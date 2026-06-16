#include "rtv/AssetImport.h"

#include "rtv/AnimationController.h"
#include "rtv/AssetManager.h"
#include "rtv/GltfLoader.h"
#include "rtv/NativeAssetCooker.h"
#include "rtv/NativeBinaryIO.h"
#include "rtv/SceneCache.h"
#include "rtv/TextureLoader.h"

#include <stb_image.h>

#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <exception>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#if RTV_ENABLE_TINYOBJ_IMPORTER && RTV_TINYOBJ_IMPORTER_AVAILABLE
#define TINYOBJLOADER_IMPLEMENTATION
#define TINYOBJLOADER_DISABLE_FAST_FLOAT
#include <tiny_obj_loader.h>
#endif

#if RTV_ENABLE_ASSIMP_IMPORTER && RTV_ASSIMP_IMPORTER_AVAILABLE
#include <assimp/Importer.hpp>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformable.h>
#endif

namespace rtv {

namespace {

std::string lowerString(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool importTraceEnabled() {
    static const bool enabled = [] {
        char* value = nullptr;
        size_t size = 0;
        const errno_t err = _dupenv_s(&value, &size, "RTV_IMPORT_TRACE");
        const bool hasValue = err == 0 && value != nullptr;
        std::free(value);
        return hasValue;
    }();
    return enabled;
}

void traceImport(std::string_view message) {
    if (importTraceEnabled()) {
        std::cerr << "[asset-import] " << message << '\n';
    }
}

std::vector<std::string> splitPathList(const char* value) {
    std::vector<std::string> paths;
    if (value == nullptr || *value == '\0') {
        return paths;
    }
    std::string token;
    std::istringstream stream(value);
    while (std::getline(stream, token, ';')) {
        if (!token.empty()) {
            paths.push_back(token);
        }
    }
    return paths;
}

#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
void registerOpenUsdPluginsFromEnvironment() {
    static const bool registered = [] {
        char* value = nullptr;
        size_t size = 0;
        const errno_t err = _dupenv_s(&value, &size, "RTV_OPENUSD_PLUGIN_PATHS");
        const std::vector<std::string> paths = err == 0 ? splitPathList(value) : std::vector<std::string>{};
        std::free(value);
        if (!paths.empty()) {
            traceImport("USD import: registering explicit plugin paths");
            pxr::PlugRegistry::GetInstance().RegisterPlugins(paths);
        }
        return true;
    }();
    (void)registered;
}

pxr::UsdStageRefPtr openUsdStageForImport(const std::filesystem::path& sourcePath) {
    registerOpenUsdPluginsFromEnvironment();
    return pxr::UsdStage::Open(sourcePath.string());
}
#endif

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

std::string projectRelativePathOrEmpty(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (path.empty() || root.empty()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path absolute = path.is_absolute() ? path : root / path;
    const std::filesystem::path relative = std::filesystem::relative(absolute, root, ec);
    if (ec || relative.empty()) {
        return {};
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return {};
        }
    }
    return relative.generic_string();
}

nlohmann::json vec3Json(glm::vec3 value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

nlohmann::json quatJson(glm::quat value) {
    const glm::quat q = glm::normalize(value);
    return nlohmann::json::array({q.x, q.y, q.z, q.w});
}

glm::vec3 scaleFromMatrix(const glm::mat4& matrix) {
    return {
        glm::length(glm::vec3(matrix[0])),
        glm::length(glm::vec3(matrix[1])),
        glm::length(glm::vec3(matrix[2])),
    };
}

glm::vec3 eulerFromMatrix(const glm::mat4& matrix) {
    glm::vec3 scale = scaleFromMatrix(matrix);
    glm::mat3 rotation{matrix};
    if (scale.x > 0.0f) {
        rotation[0] /= scale.x;
    }
    if (scale.y > 0.0f) {
        rotation[1] /= scale.y;
    }
    if (scale.z > 0.0f) {
        rotation[2] /= scale.z;
    }
    return glm::eulerAngles(glm::quat_cast(rotation));
}

nlohmann::json transformJsonFromMatrix(const glm::mat4& matrix) {
    return {
        {"position", vec3Json(glm::vec3(matrix[3]))},
        {"rotationEuler", vec3Json(eulerFromMatrix(matrix))},
        {"scale", vec3Json(scaleFromMatrix(matrix))},
    };
}

nlohmann::json matrixJson(const glm::mat4& matrix) {
    nlohmann::json result = nlohmann::json::array();
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            result.push_back(matrix[col][row]);
        }
    }
    return result;
}

nlohmann::json thumbnailMetadataJson(
    std::string kind,
    const std::string& path,
    const std::string& sourceHash,
    const std::string& importSettingsHash,
    const std::string& payloadHash = {}) {
    return {
        {"schema", "TransparentAssetThumbnailV1"},
        {"kind", std::move(kind)},
        {"path", path},
        {"available", !path.empty()},
        {"generated", false},
        {"sourceHash", sourceHash},
        {"importSettingsHash", importSettingsHash},
        {"payloadHash", payloadHash},
        {"invalidation", {
            {"sourceHash", sourceHash},
            {"importSettingsHash", importSettingsHash},
            {"payloadHash", payloadHash},
        }},
    };
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

std::string fileHashString(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    uint64_t hash = 14695981039346656037ull;
    char buffer[64 * 1024];
    while (file.good()) {
        file.read(buffer, sizeof(buffer));
        const std::streamsize count = file.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::string fileFingerprintString(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return {};
    }
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    const auto ticks = ec ? 0ll : writeTime.time_since_epoch().count();
    std::ostringstream key;
    key << path.lexically_normal().generic_string() << ':' << size << ':' << ticks;
    return pathHashString(key.str());
}

uintmax_t fileSizeOrZero(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0u : size;
}

std::optional<NativeAssetCookResult> reusableNativeCookResult(
    const std::filesystem::path& path,
    NativeAssetKind expectedKind,
    const AssetGuid& expectedGuid,
    const std::filesystem::path& sourceDependency = {}) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    if (!sourceDependency.empty() && std::filesystem::is_regular_file(sourceDependency, ec)) {
        const auto cacheTime = std::filesystem::last_write_time(path, ec);
        if (ec) {
            return std::nullopt;
        }
        const auto sourceTime = std::filesystem::last_write_time(sourceDependency, ec);
        if (ec || cacheTime < sourceTime) {
            return std::nullopt;
        }
    }
    const NativeAssetReader reader;
    const NativeAssetInspection inspection = reader.inspect(path, true);
    if (!inspection.ok || !inspection.payloadHashValid ||
        inspection.header.assetKind != static_cast<uint32_t>(expectedKind) ||
        inspection.header.assetGuid != nativeGuidFromText(expectedGuid)) {
        return std::nullopt;
    }
    NativeAssetCookResult result;
    result.success = true;
    result.path = path;
    result.payloadHash = nativeHashHex(inspection.header.payloadHash);
    result.payloadBytes = static_cast<uint64_t>(fileSizeOrZero(path));
    result.warnings.push_back("Reused existing validated native payload from cache.");
    return result;
}

template <typename T>
void appendHashPod(std::vector<std::byte>& bytes, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* raw = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

template <typename T>
void appendHashVector(std::vector<std::byte>& bytes, const std::vector<T>& values) {
    static_assert(std::is_trivially_copyable_v<T>);
    const uint64_t count = static_cast<uint64_t>(values.size());
    appendHashPod(bytes, count);
    if (!values.empty()) {
        const auto* raw = reinterpret_cast<const std::byte*>(values.data());
        bytes.insert(bytes.end(), raw, raw + sizeof(T) * values.size());
    }
}

void appendHashString(std::vector<std::byte>& bytes, std::string_view value) {
    const uint64_t count = static_cast<uint64_t>(value.size());
    appendHashPod(bytes, count);
    if (!value.empty()) {
        const auto* raw = reinterpret_cast<const std::byte*>(value.data());
        bytes.insert(bytes.end(), raw, raw + value.size());
    }
}

std::string normalizedJsonHash(const nlohmann::json& value) {
    return nativeHashHex(nativeHashText(value.dump()));
}

void appendHashVec2(std::vector<std::byte>& bytes, glm::vec2 value) {
    appendHashPod(bytes, value.x);
    appendHashPod(bytes, value.y);
}

void appendHashVec3(std::vector<std::byte>& bytes, glm::vec3 value) {
    appendHashPod(bytes, value.x);
    appendHashPod(bytes, value.y);
    appendHashPod(bytes, value.z);
}

void appendHashVec4(std::vector<std::byte>& bytes, glm::vec4 value) {
    appendHashPod(bytes, value.x);
    appendHashPod(bytes, value.y);
    appendHashPod(bytes, value.z);
    appendHashPod(bytes, value.w);
}

void appendHashTextureTransform(std::vector<std::byte>& bytes, const TextureTransformAsset& transform) {
    appendHashPod(bytes, transform.enabled);
    appendHashVec2(bytes, transform.offset);
    appendHashVec2(bytes, transform.scale);
    appendHashPod(bytes, transform.rotation);
    appendHashPod(bytes, transform.texCoord);
}

std::string meshGeometryDedupKey(const MeshAsset& mesh) {
    std::vector<std::byte> bytes;
    bytes.reserve(
        sizeof(uint64_t) * 4u +
        sizeof(MeshVertex) * mesh.vertices.size() +
        sizeof(uint32_t) * mesh.indices.size() +
        sizeof(uint32_t) * 6u * mesh.primitives.size());
    const uint64_t version = 1u;
    appendHashPod(bytes, version);
    appendHashVector(bytes, mesh.vertices);
    appendHashVector(bytes, mesh.indices);
    const uint64_t primitiveCount = static_cast<uint64_t>(mesh.primitives.size());
    appendHashPod(bytes, primitiveCount);
    for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
        appendHashPod(bytes, primitive.firstIndex);
        appendHashPod(bytes, primitive.indexCount);
        appendHashPod(bytes, primitive.firstVertex);
        appendHashPod(bytes, primitive.vertexCount);
    }
    appendHashVector(bytes, mesh.defaultMorphWeights);
    return nativeHashHex(nativeHashBytes(bytes));
}

std::string meshNativePayloadDedupKey(
    const MeshAsset& mesh,
    const std::vector<AssetGuid>& materialGuids,
    bool buildLocalBvh) {
    std::vector<std::byte> bytes;
    bytes.reserve(1024u + mesh.primitives.size() * 64u + materialGuids.size() * 40u);
    const uint64_t version = 1u;
    appendHashPod(bytes, version);
    appendHashString(bytes, meshGeometryDedupKey(mesh));
    appendHashPod(bytes, buildLocalBvh);

    const uint64_t primitiveCount = static_cast<uint64_t>(mesh.primitives.size());
    appendHashPod(bytes, primitiveCount);
    for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
        const uint32_t materialIndex = primitive.material.valid() ? primitive.material.index : UINT32_MAX;
        appendHashPod(bytes, materialIndex);
        appendHashPod(bytes, primitive.alphaMode);
        appendHashPod(bytes, primitive.alphaCutoff);
        appendHashPod(bytes, primitive.containsAlphaTestedGeometry);
        appendHashPod(bytes, primitive.containsBlendedGeometry);
        const uint64_t variantCount = static_cast<uint64_t>(primitive.materialVariants.size());
        appendHashPod(bytes, variantCount);
        for (const MeshPrimitiveAsset::MaterialVariant& variant : primitive.materialVariants) {
            appendHashPod(bytes, variant.variantIndex);
            appendHashString(bytes, variant.variantName);
            const uint32_t variantMaterialIndex = variant.material.valid() ? variant.material.index : UINT32_MAX;
            appendHashPod(bytes, variantMaterialIndex);
        }
        const uint64_t morphTargetCount = static_cast<uint64_t>(primitive.morphTargets.size());
        appendHashPod(bytes, morphTargetCount);
        for (const MeshPrimitiveAsset::MorphTarget& morphTarget : primitive.morphTargets) {
            appendHashString(bytes, morphTarget.name);
            appendHashVector(bytes, morphTarget.positionDeltas);
            appendHashVector(bytes, morphTarget.normalDeltas);
            appendHashVector(bytes, morphTarget.tangentDeltas);
        }
    }

    const uint64_t materialSlotCount = static_cast<uint64_t>(materialGuids.size());
    appendHashPod(bytes, materialSlotCount);
    for (const AssetGuid& materialGuid : materialGuids) {
        appendHashString(bytes, materialGuid);
    }
    return nativeHashHex(nativeHashBytes(bytes));
}

struct CookedAssetReuseEntry {
    AssetGuid guid;
    std::string name;
    std::filesystem::path importedPath;
    std::filesystem::path nativePath;
    size_t sourceIndex = 0;
};

std::string materialContentDedupKey(const MaterialAsset& material, const std::vector<AssetGuid>& textureGuids) {
    std::vector<std::byte> bytes;
    bytes.reserve(1024u + textureGuids.size() * 40u);
    const uint64_t version = 1u;
    appendHashPod(bytes, version);

    appendHashVec4(bytes, material.baseColorFactor);
    appendHashVec3(bytes, material.emissiveFactor);
    appendHashPod(bytes, material.metallicFactor);
    appendHashPod(bytes, material.roughnessFactor);
    appendHashPod(bytes, material.iorFactor);
    appendHashPod(bytes, material.alphaCutoff);
    appendHashPod(bytes, material.alphaMode);
    appendHashPod(bytes, material.doubleSided);
    appendHashPod(bytes, material.hasIor);
    appendHashPod(bytes, material.hasClearcoat);
    appendHashPod(bytes, material.clearcoatFactor);
    appendHashPod(bytes, material.clearcoatRoughnessFactor);
    appendHashPod(bytes, material.hasTransmission);
    appendHashPod(bytes, material.transmissionFactor);
    appendHashPod(bytes, material.hasVolume);
    appendHashPod(bytes, material.volumeThicknessFactor);
    appendHashPod(bytes, material.volumeAttenuationDistance);
    appendHashVec3(bytes, material.volumeAttenuationColor);
    appendHashPod(bytes, material.nestedPriority);
    appendHashPod(bytes, material.hasDispersion);
    appendHashPod(bytes, material.dispersionFactor);
    appendHashPod(bytes, material.hasSpecular);
    appendHashPod(bytes, material.specularFactor);
    appendHashVec3(bytes, material.specularColorFactor);
    appendHashPod(bytes, material.hasSheen);
    appendHashVec3(bytes, material.sheenColorFactor);
    appendHashPod(bytes, material.sheenRoughnessFactor);
    appendHashPod(bytes, material.hasIridescence);
    appendHashPod(bytes, material.iridescenceFactor);
    appendHashPod(bytes, material.iridescenceIor);
    appendHashPod(bytes, material.iridescenceThicknessMinimum);
    appendHashPod(bytes, material.iridescenceThicknessMaximum);
    appendHashPod(bytes, material.hasEmissiveStrength);
    appendHashPod(bytes, material.emissiveStrength);
    appendHashPod(bytes, material.hasAnisotropy);
    appendHashPod(bytes, material.anisotropyStrength);
    appendHashPod(bytes, material.anisotropyRotation);
    appendHashPod(bytes, material.occlusionStrength);
    appendHashPod(bytes, material.useConductorOptics);
    appendHashVec3(bytes, material.conductorEta);
    appendHashVec3(bytes, material.conductorK);
    appendHashPod(bytes, material.heightScale);
    appendHashPod(bytes, material.materialWorkflow);
    appendHashPod(bytes, material.normalMapConvention);
    appendHashPod(bytes, material.specularTextureAlphaMode);
    appendHashPod(bytes, material.shaderCompatibilityMask);

    auto appendTextureSlot = [&](TextureAssetHandle handle) {
        const uint32_t bound = handle.valid() && handle.index < textureGuids.size() ? 1u : 0u;
        appendHashPod(bytes, bound);
        if (bound != 0u) {
            appendHashString(bytes, textureGuids[handle.index]);
        }
    };
    appendTextureSlot(material.baseColorTexture);
    appendTextureSlot(material.normalTexture);
    appendTextureSlot(material.metallicRoughnessTexture);
    appendTextureSlot(material.occlusionTexture);
    appendTextureSlot(material.emissiveTexture);
    appendTextureSlot(material.transmissionTexture);
    appendTextureSlot(material.clearcoatTexture);
    appendTextureSlot(material.clearcoatRoughnessTexture);
    appendTextureSlot(material.clearcoatNormalTexture);
    appendTextureSlot(material.volumeThicknessTexture);
    appendTextureSlot(material.sheenColorTexture);
    appendTextureSlot(material.sheenRoughnessTexture);
    appendTextureSlot(material.specularTexture);
    appendTextureSlot(material.specularColorTexture);
    appendTextureSlot(material.iridescenceTexture);
    appendTextureSlot(material.iridescenceThicknessTexture);
    appendTextureSlot(material.anisotropyTexture);
    appendTextureSlot(material.opacityTexture);
    appendTextureSlot(material.heightTexture);

    appendHashTextureTransform(bytes, material.baseColorTextureTransform);
    appendHashTextureTransform(bytes, material.normalTextureTransform);
    appendHashTextureTransform(bytes, material.metallicRoughnessTextureTransform);
    appendHashTextureTransform(bytes, material.emissiveTextureTransform);
    appendHashTextureTransform(bytes, material.occlusionTextureTransform);
    appendHashTextureTransform(bytes, material.sheenColorTextureTransform);
    appendHashTextureTransform(bytes, material.sheenRoughnessTextureTransform);
    appendHashTextureTransform(bytes, material.iridescenceTextureTransform);
    appendHashTextureTransform(bytes, material.iridescenceThicknessTextureTransform);
    appendHashTextureTransform(bytes, material.clearcoatTextureTransform);
    appendHashTextureTransform(bytes, material.clearcoatRoughnessTextureTransform);
    appendHashTextureTransform(bytes, material.clearcoatNormalTextureTransform);
    appendHashTextureTransform(bytes, material.transmissionTextureTransform);
    appendHashTextureTransform(bytes, material.volumeThicknessTextureTransform);
    appendHashTextureTransform(bytes, material.specularTextureTransform);
    appendHashTextureTransform(bytes, material.specularColorTextureTransform);
    appendHashTextureTransform(bytes, material.anisotropyTextureTransform);

    return nativeHashHex(nativeHashBytes(bytes));
}

std::filesystem::path destinationFolderForType(AssetType type) {
    switch (type) {
    case AssetType::HDRI: return "HDRI";
    case AssetType::Texture: return "Textures";
    case AssetType::Material: return "Materials";
    case AssetType::Mesh:
    case AssetType::Prefab: return "Models";
    case AssetType::Scene: return "Scenes";
    case AssetType::Animation: return "Animations";
    case AssetType::AnimationController: return "AnimationControllers";
    case AssetType::Skeleton: return "Skeletons";
    case AssetType::SkeletalMesh: return "Skeletons";
    case AssetType::Unknown:
    default: return "Models";
    }
}

std::string importedAssetExtensionForType(AssetType type) {
    switch (type) {
    case AssetType::Prefab: return ".rtprefab.json";
    case AssetType::Texture: return ".rttexture.json";
    case AssetType::HDRI: return ".rthdri.json";
    case AssetType::Material: return ".rtmaterial.json";
    case AssetType::Mesh: return ".rtmesh.json";
    case AssetType::Scene: return ".rtlevelasset.json";
    case AssetType::Animation: return ".rtanim.json";
    case AssetType::AnimationController: return ".rtanimcontroller.json";
    case AssetType::Skeleton: return ".rtskeleton.json";
    case AssetType::SkeletalMesh: return ".rtskeletalmesh.json";
    case AssetType::Unknown:
    default: return ".rtasset.json";
    }
}

std::string standaloneAssetKindForType(AssetType type) {
    switch (type) {
    case AssetType::Texture: return "ImportedTexture";
    case AssetType::HDRI: return "ImportedHDRI";
    case AssetType::Material: return "ImportedMtlMaterialLibrary";
    case AssetType::Animation: return "ImportedAnimation";
    case AssetType::AnimationController: return "ImportedAnimationController";
    case AssetType::Skeleton: return "ImportedSkeleton";
    case AssetType::SkeletalMesh: return "ImportedSkeletalMeshBinding";
    case AssetType::Scene: return "ImportedSceneSource";
    default: return "PlaceholderImportedAsset";
    }
}

std::string importerLabelForType(AssetType type) {
    switch (type) {
    case AssetType::Prefab: return "GltfLoader";
    case AssetType::Texture: return "TextureLoader";
    case AssetType::HDRI: return "HDRTextureLoader";
    case AssetType::Material: return "MtlMetadataImporter";
    case AssetType::Scene: return "SceneMetadataImporter";
    default: return "PlaceholderImporter";
    }
}

std::string importCacheKindForType(AssetType type) {
    switch (type) {
    case AssetType::Prefab: return "GltfImportCacheSummary";
    case AssetType::Texture: return "TextureImportCacheSummary";
    case AssetType::HDRI: return "HDRIImportCacheSummary";
    case AssetType::Material: return "MtlImportCacheSummary";
    case AssetType::Scene: return "SceneImportCacheSummary";
    default: return "PlaceholderImportCache";
    }
}

std::string textureFormatLabel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return "BC1_RGB_UNORM";
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return "BC1_RGB_SRGB";
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1_RGBA_UNORM";
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "BC1_RGBA_SRGB";
    case VK_FORMAT_BC2_UNORM_BLOCK: return "BC2_UNORM";
    case VK_FORMAT_BC2_SRGB_BLOCK: return "BC2_SRGB";
    case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3_UNORM";
    case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3_SRGB";
    case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4_UNORM";
    case VK_FORMAT_BC4_SNORM_BLOCK: return "BC4_SNORM";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5_UNORM";
    case VK_FORMAT_BC5_SNORM_BLOCK: return "BC5_SNORM";
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: return "BC6H_UFLOAT";
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return "BC6H_SFLOAT";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7_UNORM";
    case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7_SRGB";
    case VK_FORMAT_UNDEFINED: return "UNDEFINED";
    default: return "VkFormat_" + std::to_string(static_cast<uint32_t>(format));
    }
}

std::string textureColorSpaceLabel(const TextureAsset& texture) {
    if (texture.srgb) return "sRGB";
    return texture.linearColorSpace ? "Linear" : "DataLinear";
}

std::string materialAlphaModeLabel(uint32_t alphaMode);
std::string materialWorkflowLabel(uint32_t workflow);
std::string materialNormalMapConventionLabel(uint32_t convention);
std::string materialSpecularTextureAlphaModeLabel(uint32_t mode);

nlohmann::json vec3ArrayJson(glm::vec3 value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

nlohmann::json vec4ArrayJson(glm::vec4 value) {
    return nlohmann::json::array({value.x, value.y, value.z, value.w});
}

nlohmann::json textureTransformJson(const TextureTransformAsset& transform) {
    return {
        {"enabled", transform.enabled != 0u},
        {"offset", nlohmann::json::array({transform.offset.x, transform.offset.y})},
        {"scale", nlohmann::json::array({transform.scale.x, transform.scale.y})},
        {"rotation", transform.rotation},
        {"texCoord", transform.texCoord},
    };
}

nlohmann::json materialConversionDiagnosticsJson(const MaterialAsset& material) {
    // Honest, code-derived record of conversions that lose information relative to the
    // source authoring model, plus features the runtime PBR closure cannot represent.
    // Computed purely from the resolved MaterialAsset so it matches what is actually cooked.
    nlohmann::json lossyConversions = nlohmann::json::array();
    nlohmann::json unsupportedSourceFeatures = nlohmann::json::array();
    for (const std::string& feature : material.unsupportedSourceFeatures) {
        unsupportedSourceFeatures.push_back(feature);
    }

    if (material.materialWorkflow == kMaterialWorkflowSpecularGlossiness) {
        lossyConversions.push_back({
            {"feature", "KHR_materials_pbrSpecularGlossiness"},
            {"conversion", "specular_glossiness_to_metallic_roughness"},
            {"detail", "glossiness mapped to roughness=1-glossiness; diffuse used as baseColor; metallic forced to 0; specularGlossiness texture alpha reinterpreted as glossiness"},
            {"reversible", false},
        });
    }
    if (material.materialWorkflow == kMaterialWorkflowPackedOcclusionRoughnessMetalness) {
        lossyConversions.push_back({
            {"feature", "packed_occlusion_roughness_metalness"},
            {"conversion", "packed_orm_channel_unpack"},
            {"detail", "FBX packed ORM texture channels interpreted as occlusion(R)/roughness(G)/metalness(B)"},
            {"reversible", true},
        });
    }
    if (material.specularTextureAlphaMode == kMaterialSpecularTextureAlphaGlossiness) {
        lossyConversions.push_back({
            {"feature", "specular_texture_alpha_glossiness"},
            {"conversion", "specular_alpha_to_roughness"},
            {"detail", "specular map alpha channel reinterpreted as glossiness then inverted to roughness"},
            {"reversible", false},
        });
    }

    return {
        {"lossyConversions", lossyConversions},
        {"lossyConversionCount", lossyConversions.size()},
        {"unsupportedSourceFeatures", unsupportedSourceFeatures},
        {"unsupportedSourceFeatureCount", unsupportedSourceFeatures.size()},
        {"deterministicFallback", true},
    };
}

nlohmann::json materialPbrMetadataJson(const MaterialAsset& material) {
    return {
        {"conversionDiagnostics", materialConversionDiagnosticsJson(material)},
        {"workflow", materialWorkflowLabel(material.materialWorkflow)},
        {"normalMapConvention", materialNormalMapConventionLabel(material.normalMapConvention)},
        {"specularTextureAlphaMode", materialSpecularTextureAlphaModeLabel(material.specularTextureAlphaMode)},
        {"baseColorFactor", vec4ArrayJson(material.baseColorFactor)},
        {"metallicFactor", material.metallicFactor},
        {"roughnessFactor", material.roughnessFactor},
        {"emissiveFactor", vec3ArrayJson(material.emissiveFactor)},
        {"emissiveStrength", material.emissiveStrength},
        {"alpha", {
            {"mode", materialAlphaModeLabel(material.alphaMode)},
            {"cutoff", material.alphaCutoff},
            {"doubleSided", material.doubleSided != 0u},
        }},
        {"occlusionStrength", material.occlusionStrength},
        {"heightScale", material.heightScale},
        {"shaderCompatibilityMask", material.shaderCompatibilityMask},
        {"extensions", {
            {"ior", {"present", material.hasIor != 0u, "ior", material.iorFactor}},
            {"clearcoat", {"present", material.hasClearcoat != 0u, "factor", material.clearcoatFactor, "roughnessFactor", material.clearcoatRoughnessFactor}},
            {"transmission", {"present", material.hasTransmission != 0u, "factor", material.transmissionFactor}},
            {"volume", {"present", material.hasVolume != 0u, "thicknessFactor", material.volumeThicknessFactor, "attenuationDistance", material.volumeAttenuationDistance, "attenuationColor", vec3ArrayJson(material.volumeAttenuationColor)}},
            {"nestedPriority", material.nestedPriority},
            {"dispersion", {"present", material.hasDispersion != 0u, "dispersion", material.dispersionFactor}},
            {"specular", {"present", material.hasSpecular != 0u, "factor", material.specularFactor, "colorFactor", vec3ArrayJson(material.specularColorFactor)}},
            {"sheen", {"present", material.hasSheen != 0u, "colorFactor", vec3ArrayJson(material.sheenColorFactor), "roughnessFactor", material.sheenRoughnessFactor}},
            {"iridescence", {"present", material.hasIridescence != 0u, "factor", material.iridescenceFactor, "ior", material.iridescenceIor, "thicknessMinimum", material.iridescenceThicknessMinimum, "thicknessMaximum", material.iridescenceThicknessMaximum}},
            {"anisotropy", {"present", material.hasAnisotropy != 0u, "strength", material.anisotropyStrength, "rotation", material.anisotropyRotation}},
        }},
        {"textureTransforms", {
            {"baseColor", textureTransformJson(material.baseColorTextureTransform)},
            {"metallicRoughness", textureTransformJson(material.metallicRoughnessTextureTransform)},
            {"normal", textureTransformJson(material.normalTextureTransform)},
            {"emissive", textureTransformJson(material.emissiveTextureTransform)},
            {"occlusion", textureTransformJson(material.occlusionTextureTransform)},
            {"clearcoat", textureTransformJson(material.clearcoatTextureTransform)},
            {"clearcoatRoughness", textureTransformJson(material.clearcoatRoughnessTextureTransform)},
            {"clearcoatNormal", textureTransformJson(material.clearcoatNormalTextureTransform)},
            {"transmission", textureTransformJson(material.transmissionTextureTransform)},
            {"volumeThickness", textureTransformJson(material.volumeThicknessTextureTransform)},
            {"specular", textureTransformJson(material.specularTextureTransform)},
            {"specularColor", textureTransformJson(material.specularColorTextureTransform)},
            {"sheenColor", textureTransformJson(material.sheenColorTextureTransform)},
            {"sheenRoughness", textureTransformJson(material.sheenRoughnessTextureTransform)},
            {"iridescence", textureTransformJson(material.iridescenceTextureTransform)},
            {"iridescenceThickness", textureTransformJson(material.iridescenceThicknessTextureTransform)},
            {"anisotropy", textureTransformJson(material.anisotropyTextureTransform)},
        }},
    };
}

nlohmann::json inferTextureRole(const std::filesystem::path& sourcePath, AssetType type) {
    if (type == AssetType::HDRI) {
        return {
            {"role", "environmentHDR"},
            {"colorSpace", "Linear"},
            {"confidence", "format"},
            {"matchedToken", lowerString(sourcePath.extension().string())},
        };
    }

    std::string name = lowerString(sourcePath.stem().string());
    for (char& c : name) {
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9')) {
            c = '_';
        }
    }
    const std::string padded = "_" + name + "_";
    auto hasToken = [&](std::initializer_list<const char*> tokens) -> const char* {
        for (const char* token : tokens) {
            const std::string needle = std::string("_") + token + "_";
            if (padded.find(needle) != std::string::npos) {
                return token;
            }
        }
        return nullptr;
    };

    struct RoleRule {
        const char* role;
        const char* colorSpace;
        std::initializer_list<const char*> tokens;
    };
    static const RoleRule rules[] = {
        {"normal", "Linear", {"normal", "norm", "nrm", "nor", "n"}},
        {"metallicRoughness", "Linear", {"metallicroughness", "metalrough", "metal_rough", "orm", "rma", "mra", "mrao", "rmao"}},
        {"roughness", "Linear", {"roughness", "rough", "rgh"}},
        {"metallic", "Linear", {"metallic", "metalness", "metal", "met"}},
        {"occlusion", "Linear", {"occlusion", "ambientocclusion", "ao", "occ"}},
        {"emissive", "sRGB", {"emissive", "emission", "emit", "glow"}},
        {"decal", "sRGB", {"decal", "sticker", "decalcolor", "decal_color"}},
        {"label", "sRGB", {"label", "labels", "logo", "logos", "textlabel", "text_label"}},
        {"opacity", "Linear", {"opacity", "alpha", "transparency", "mask"}},
        {"height", "Linear", {"height", "displacement", "disp", "bump"}},
        {"data", "Linear", {"data", "maskdata", "lookup", "lut"}},
        {"baseColor", "sRGB", {"basecolor", "base_color", "albedo", "diffuse", "diff", "color", "col", "bc"}},
    };
    for (const RoleRule& rule : rules) {
        if (const char* token = hasToken(rule.tokens)) {
            return {
                {"role", rule.role},
                {"colorSpace", rule.colorSpace},
                {"confidence", "filename_token"},
                {"matchedToken", token},
            };
        }
    }
    return {
        {"role", "unknown"},
        {"colorSpace", "sRGB"},
        {"confidence", "none"},
        {"matchedToken", ""},
    };
}

std::string materialAlphaModeLabel(uint32_t alphaMode) {
    switch (alphaMode) {
    case kMaterialAlphaModeMask: return "Mask";
    case kMaterialAlphaModeBlend: return "Blend";
    case kMaterialAlphaModeOpaque:
    default: return "Opaque";
    }
}

// Build a texture-index -> authored NativeTextureRole map from material slot bindings.
// The material slot bindings survive the scene cache round-trip (unlike a per-texture
// authored-role field), so this recovers the authoritative role for both the fresh-load
// and cached-load import paths from a single source of truth. When two materials bind the
// same texture to different slots, the higher-priority (more color-space-significant) role
// wins, matching the loader's role-priority resolution.
std::unordered_map<uint32_t, NativeTextureRole> buildTextureRolesFromMaterials(
    const std::vector<MaterialAsset>& materials) {
    auto rolePriority = [](NativeTextureRole role) -> int {
        switch (role) {
        case NativeTextureRole::Normal:
        case NativeTextureRole::ClearcoatNormal: return 6;
        case NativeTextureRole::BaseColor:
        case NativeTextureRole::Emissive:
        case NativeTextureRole::SpecularColor:
        case NativeTextureRole::SheenColor: return 5;
        case NativeTextureRole::MetallicRoughness: return 4;
        case NativeTextureRole::Occlusion:
        case NativeTextureRole::Opacity:
        case NativeTextureRole::Roughness:
        case NativeTextureRole::ClearcoatRoughness:
        case NativeTextureRole::SheenRoughness: return 3;
        case NativeTextureRole::Specular:
        case NativeTextureRole::Transmission:
        case NativeTextureRole::Clearcoat:
        case NativeTextureRole::Sheen:
        case NativeTextureRole::Iridescence:
        case NativeTextureRole::IridescenceThickness:
        case NativeTextureRole::Anisotropy:
        case NativeTextureRole::Height:
        case NativeTextureRole::Thickness:
        case NativeTextureRole::Data: return 2;
        case NativeTextureRole::Unknown: return 0;
        default: return 1;
        }
    };
    std::unordered_map<uint32_t, NativeTextureRole> roles;
    auto bind = [&](const TextureAssetHandle& handle, NativeTextureRole role) {
        if (!handle.valid() || role == NativeTextureRole::Unknown) {
            return;
        }
        auto it = roles.find(handle.index);
        if (it == roles.end() || rolePriority(role) > rolePriority(it->second)) {
            roles[handle.index] = role;
        }
    };
    for (const MaterialAsset& material : materials) {
        bind(material.baseColorTexture, NativeTextureRole::BaseColor);
        bind(material.normalTexture, NativeTextureRole::Normal);
        bind(material.metallicRoughnessTexture, NativeTextureRole::MetallicRoughness);
        bind(material.emissiveTexture, NativeTextureRole::Emissive);
        bind(material.occlusionTexture, NativeTextureRole::Occlusion);
        bind(material.opacityTexture, NativeTextureRole::Opacity);
        bind(material.heightTexture, NativeTextureRole::Height);
        bind(material.clearcoatTexture, NativeTextureRole::Clearcoat);
        bind(material.clearcoatRoughnessTexture, NativeTextureRole::ClearcoatRoughness);
        bind(material.clearcoatNormalTexture, NativeTextureRole::ClearcoatNormal);
        bind(material.transmissionTexture, NativeTextureRole::Transmission);
        bind(material.volumeThicknessTexture, NativeTextureRole::Thickness);
        bind(material.specularTexture, NativeTextureRole::Specular);
        bind(material.specularColorTexture, NativeTextureRole::SpecularColor);
        bind(material.sheenColorTexture, NativeTextureRole::SheenColor);
        bind(material.sheenRoughnessTexture, NativeTextureRole::SheenRoughness);
        bind(material.iridescenceTexture, NativeTextureRole::Iridescence);
        bind(material.iridescenceThicknessTexture, NativeTextureRole::IridescenceThickness);
        bind(material.anisotropyTexture, NativeTextureRole::Anisotropy);
    }
    return roles;
}

std::string materialWorkflowLabel(uint32_t workflow) {
    switch (workflow) {
    case kMaterialWorkflowPackedOcclusionRoughnessMetalness:
        return "FBX packed occlusion-roughness-metalness";
    case kMaterialWorkflowSpecularGlossiness:
        return "Specular-glossiness";
    case kMaterialWorkflowMetallicRoughness:
    default:
        return "Metallic-roughness";
    }
}

std::string materialNormalMapConventionLabel(uint32_t convention) {
    switch (convention) {
    case kMaterialNormalMapDirectX: return "DirectX";
    case kMaterialNormalMapOpenGL:
    default: return "OpenGL";
    }
}

std::string materialSpecularTextureAlphaModeLabel(uint32_t mode) {
    switch (mode) {
    case kMaterialSpecularTextureAlphaGlossiness: return "Glossiness";
    case kMaterialSpecularTextureAlphaNone:
    default: return "None";
    }
}

nlohmann::json sourceControlPolicyJson(bool copiedSourceIntoProject) {
    return {
        {"schema", "TransparentAssetMetadataV1"},
        {"commitImportedMetadata", true},
        {"commitSourceAssets", copiedSourceIntoProject},
        {"commitCookedPayloads", false},
        {"commitThumbnails", false},
        {"regenerateCookedPayloadsWhenMissing", true},
        {"notes", copiedSourceIntoProject
            ? "Commit Content metadata and copied SourceAssets. Cache/runtime payloads are generated data and can be regenerated."
            : "Commit Content metadata. External source paths are references; Cache/runtime payloads are generated data and can be regenerated."},
    };
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string decodeUriPath(std::string_view uri) {
    std::string decoded;
    decoded.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int hi = hexValue(uri[i + 1]);
            const int lo = hexValue(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(static_cast<char>(uri[i]));
    }
    return decoded;
}

bool isExternalOrDataUri(std::string_view uri) {
    return uri.find("://") != std::string_view::npos || uri.rfind("data:", 0) == 0;
}

bool isDataUri(std::string_view uri) {
    return uri.rfind("data:", 0) == 0;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::optional<std::vector<uint8_t>> decodeDataUriPayload(std::string_view uri) {
    if (!isDataUri(uri)) {
        return std::nullopt;
    }
    const size_t comma = uri.find(',');
    if (comma == std::string_view::npos) {
        return std::nullopt;
    }
    std::string header = lowerString(std::string(uri.substr(0, comma)));
    const std::string_view payload = uri.substr(comma + 1);
    if (header.find(";base64") == std::string::npos) {
        const std::string decoded = decodeUriPath(payload);
        return std::vector<uint8_t>(decoded.begin(), decoded.end());
    }

    std::vector<uint8_t> bytes;
    bytes.reserve((payload.size() * 3u) / 4u);
    int quartet[4] = {};
    int quartetCount = 0;
    for (char c : payload) {
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            continue;
        }
        quartet[quartetCount++] = c == '=' ? -2 : base64Value(c);
        if (quartet[quartetCount - 1] == -1) {
            return std::nullopt;
        }
        if (quartetCount != 4) {
            continue;
        }

        const int a = quartet[0];
        const int b = quartet[1];
        const int c2 = quartet[2];
        const int d = quartet[3];
        if (a < 0 || b < 0 || (c2 < 0 && c2 != -2) || (d < 0 && d != -2)) {
            return std::nullopt;
        }
        if (c2 == -2 && d != -2) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        if (c2 != -2) {
            bytes.push_back(static_cast<uint8_t>(((b & 0x0F) << 4) | (c2 >> 2)));
        }
        if (d != -2) {
            bytes.push_back(static_cast<uint8_t>(((c2 & 0x03) << 6) | d));
        }
        quartetCount = 0;
    }
    if (quartetCount != 0) {
        return std::nullopt;
    }
    return bytes;
}

bool pathIsWithinLexicalRoot(const std::filesystem::path& path, const std::filesystem::path& root) {
    const std::filesystem::path normalizedPath = path.lexically_normal();
    const std::filesystem::path normalizedRoot = root.lexically_normal();
    const std::filesystem::path relative = normalizedPath.lexically_relative(normalizedRoot);
    if (relative.empty()) {
        return normalizedPath == normalizedRoot;
    }
    const std::string relativeString = relative.generic_string();
    return relativeString != ".." && relativeString.rfind("../", 0) != 0;
}

bool copyImportSourceFile(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::vector<std::filesystem::path>& generatedFiles,
    std::vector<std::string>& errors,
    std::unordered_set<std::string>& copiedKeys) {
    const std::filesystem::path normalizedDestination = destination.lexically_normal();
    const std::string key = normalizedDestination.generic_string();
    if (!copiedKeys.insert(key).second) {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(normalizedDestination.parent_path(), ec);
    if (ec) {
        errors.push_back("Could not create source asset folder: " + ec.message());
        return false;
    }
    const std::filesystem::path normalizedSource = source.lexically_normal();
    if (normalizedSource != normalizedDestination) {
        std::filesystem::copy_file(normalizedSource, normalizedDestination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            errors.push_back("Could not copy source asset " + normalizedSource.string() + " to " + normalizedDestination.string() + ": " + ec.message());
            return false;
        }
    }
    generatedFiles.push_back(normalizedDestination);
    return true;
}

void copyGltfExternalReferences(
    const std::filesystem::path& originalSource,
    const std::filesystem::path& copiedSource,
    const std::filesystem::path& allowedRoot,
    std::vector<std::filesystem::path>& generatedFiles,
    std::vector<std::string>& warnings,
    std::vector<std::string>& errors,
    std::unordered_set<std::string>& copiedKeys) {
    std::ifstream file(originalSource);
    if (!file.is_open()) {
        warnings.push_back("Could not inspect glTF source dependencies while copying source assets.");
        return;
    }

    nlohmann::json gltf;
    try {
        file >> gltf;
    } catch (const std::exception& ex) {
        warnings.push_back(std::string("Could not parse glTF source dependencies while copying source assets: ") + ex.what());
        return;
    }

    auto copyUri = [&](const nlohmann::json& item) {
        if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string()) {
            return;
        }
        const std::string uri = item["uri"].get<std::string>();
        if (uri.empty() || isExternalOrDataUri(uri)) {
            return;
        }
        const std::filesystem::path relativePath = std::filesystem::path(decodeUriPath(uri)).lexically_normal();
        if (relativePath.is_absolute()) {
            warnings.push_back("Skipped absolute glTF dependency while copying source assets: " + uri);
            return;
        }
        const std::filesystem::path sourcePath = (originalSource.parent_path() / relativePath).lexically_normal();
        const std::filesystem::path destinationPath = (copiedSource.parent_path() / relativePath).lexically_normal();
        if (!pathIsWithinLexicalRoot(destinationPath, allowedRoot)) {
            warnings.push_back("Skipped glTF dependency outside SourceAssets import folder: " + uri);
            return;
        }
        (void)copyImportSourceFile(sourcePath, destinationPath, generatedFiles, errors, copiedKeys);
    };

    if (gltf.contains("buffers") && gltf["buffers"].is_array()) {
        for (const nlohmann::json& item : gltf["buffers"]) {
            copyUri(item);
        }
    }
    if (gltf.contains("images") && gltf["images"].is_array()) {
        for (const nlohmann::json& item : gltf["images"]) {
            copyUri(item);
        }
    }
}

std::string trimAscii(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string firstObjToken(std::string_view value) {
    const std::string trimmed = trimAscii(value);
    const size_t split = trimmed.find_first_of(" \t\r\n");
    return split == std::string::npos ? trimmed : trimmed.substr(0, split);
}

std::string normalizedNameTokens(std::string value) {
    value = lowerString(std::move(value));
    for (char& c : value) {
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9')) {
            c = '_';
        }
    }
    return "_" + value + "_";
}

bool hasNameToken(const std::string& normalized, std::initializer_list<const char*> tokens) {
    for (const char* token : tokens) {
        if (normalized.find(std::string("_") + token + "_") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string collisionKindForName(std::string_view name) {
    const std::string lower = lowerString(std::string(name));
    const std::string normalized = normalizedNameTokens(lower);
    if (lower.rfind("ucx_", 0) == 0 || lower.rfind("convex_", 0) == 0 || hasNameToken(normalized, {"ucx", "convex", "hull"})) {
        return "convexHull";
    }
    if (lower.rfind("ubx_", 0) == 0 || hasNameToken(normalized, {"ubx", "box"})) {
        return "box";
    }
    if (lower.rfind("usp_", 0) == 0 || hasNameToken(normalized, {"usp", "sphere"})) {
        return "sphere";
    }
    if (lower.rfind("ucp_", 0) == 0 || hasNameToken(normalized, {"ucp", "capsule"})) {
        return "capsule";
    }
    if (hasNameToken(normalized, {"collision", "collider", "collidermesh", "col"})) {
        return "collisionMesh";
    }
    return {};
}

int lodIndexForName(std::string_view name) {
    const std::string lower = lowerString(std::string(name));
    size_t pos = lower.find("lod");
    while (pos != std::string::npos) {
        size_t cursor = pos + 3;
        while (cursor < lower.size() && (lower[cursor] == '_' || lower[cursor] == '-' || lower[cursor] == ' ')) {
            ++cursor;
        }
        if (cursor < lower.size() && std::isdigit(static_cast<unsigned char>(lower[cursor]))) {
            int value = 0;
            while (cursor < lower.size() && std::isdigit(static_cast<unsigned char>(lower[cursor]))) {
                value = value * 10 + (lower[cursor] - '0');
                ++cursor;
            }
            return value;
        }
        pos = lower.find("lod", pos + 3);
    }
    return -1;
}

void appendCollisionLodNameMetadata(
    const std::string& name,
    const char* sourceKind,
    size_t sourceIndex,
    nlohmann::json& collisionCandidates,
    nlohmann::json& lodCandidates,
    std::unordered_set<std::string>& collisionKeys,
    std::unordered_set<std::string>& lodKeys) {
    if (name.empty()) {
        return;
    }
    const std::string collisionKind = collisionKindForName(name);
    if (!collisionKind.empty()) {
        const std::string key = std::string(sourceKind) + ":" + std::to_string(sourceIndex) + ":" + name + ":" + collisionKind;
        if (collisionKeys.insert(key).second) {
            collisionCandidates.push_back({
                {"name", name},
                {"sourceKind", sourceKind},
                {"sourceIndex", sourceIndex},
                {"collisionKind", collisionKind},
                {"runtimeSupport", "authored_collision_metadata_preserved"},
                {"runtimeSystemScope", "physics_collision_backend_not_part_of_asset_importer"},
            });
        }
    }
    const int lodIndex = lodIndexForName(name);
    if (lodIndex >= 0) {
        const std::string key = std::string(sourceKind) + ":" + std::to_string(sourceIndex) + ":" + name + ":" + std::to_string(lodIndex);
        if (lodKeys.insert(key).second) {
            lodCandidates.push_back({
                {"name", name},
                {"sourceKind", sourceKind},
                {"sourceIndex", sourceIndex},
                {"lodIndex", lodIndex},
                {"runtimeSupport", "authored_lod_metadata_preserved"},
                {"runtimeSystemScope", "renderer_lod_selection_not_part_of_asset_importer"},
            });
        }
    }
}

nlohmann::json collisionLodMetadataJson(
    std::string format,
    bool inspected,
    nlohmann::json collisionCandidates,
    nlohmann::json lodCandidates) {
    return {
        {"inspected", inspected},
        {"format", std::move(format)},
        {"collisionCandidateCount", collisionCandidates.is_array() ? collisionCandidates.size() : 0},
        {"lodCandidateCount", lodCandidates.is_array() ? lodCandidates.size() : 0},
        {"collisionCandidates", std::move(collisionCandidates)},
        {"lodCandidates", std::move(lodCandidates)},
        {"runtimeSupport", "authored_collision_lod_metadata_preserved"},
        {"runtimeSystemScope", "asset_importer_records_source_intent; physics_collision_and_renderer_lod_selection_are_runtime_systems"},
    };
}

std::vector<std::string> whitespaceTokens(std::string_view value) {
    std::vector<std::string> tokens;
    std::istringstream stream{std::string(value)};
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

nlohmann::json floatTokenArray(const std::vector<std::string>& tokens, size_t begin) {
    nlohmann::json values = nlohmann::json::array();
    for (size_t i = begin; i < tokens.size(); ++i) {
        try {
            values.push_back(std::stof(tokens[i]));
        } catch (...) {
            values.push_back(tokens[i]);
        }
    }
    return values;
}

void addWarningOnce(std::vector<std::string>& warnings, std::string warning) {
    if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
        warnings.push_back(std::move(warning));
    }
}

nlohmann::json jsonStringArray(const nlohmann::json& object, const char* key) {
    nlohmann::json values = nlohmann::json::array();
    if (!object.is_object() || !object.contains(key) || !object[key].is_array()) {
        return values;
    }
    for (const nlohmann::json& item : object[key]) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        }
    }
    return values;
}

bool supportedGltfExtensionForImportReport(const std::string& name) {
    static const std::unordered_set<std::string> supported = {
        "KHR_materials_clearcoat",
        "KHR_materials_transmission",
        "KHR_materials_volume",
        "KHR_materials_dispersion",
        "KHR_materials_ior",
        "KHR_materials_specular",
        "KHR_materials_pbrSpecularGlossiness",
        "KHR_materials_sheen",
        "KHR_materials_iridescence",
        "KHR_materials_emissive_strength",
        "KHR_materials_anisotropy",
        "KHR_materials_unlit",
        "KHR_materials_variants",
        "KHR_mesh_quantization",
        "KHR_texture_basisu",
        "MSFT_texture_dds",
        "KHR_lights_punctual",
        "KHR_texture_transform",
    };
    return supported.find(name) != supported.end();
}

std::string gltfPrimitiveModeLabel(int mode) {
    switch (mode) {
    case 0: return "POINTS";
    case 1: return "LINES";
    case 2: return "LINE_LOOP";
    case 3: return "LINE_STRIP";
    case 4: return "TRIANGLES";
    case 5: return "TRIANGLE_STRIP";
    case 6: return "TRIANGLE_FAN";
    default: return "UNKNOWN_" + std::to_string(mode);
    }
}

bool gltfPrimitiveModeRenderableAsTriangles(int mode) {
    return mode == 4 || mode == 5 || mode == 6;
}

constexpr int kGltfComponentByte = 5120;
constexpr int kGltfComponentUnsignedByte = 5121;
constexpr int kGltfComponentShort = 5122;
constexpr int kGltfComponentUnsignedShort = 5123;
constexpr int kGltfComponentFloat = 5126;

enum class GltfQuantizedFloatPolicy {
    PositionOrTexcoord,
    SignedPositionDelta,
    SignedNormalizedInteger,
};

bool gltfQuantizedFloatComponentType(int componentType) {
    return componentType == kGltfComponentByte ||
        componentType == kGltfComponentUnsignedByte ||
        componentType == kGltfComponentShort ||
        componentType == kGltfComponentUnsignedShort;
}

bool gltfSignedQuantizedFloatComponentType(int componentType) {
    return componentType == kGltfComponentByte || componentType == kGltfComponentShort;
}

bool gltfQuantizedFloatComponentAllowed(int componentType, bool normalized, GltfQuantizedFloatPolicy policy) {
    if (!gltfQuantizedFloatComponentType(componentType)) {
        return false;
    }
    switch (policy) {
    case GltfQuantizedFloatPolicy::PositionOrTexcoord:
        return true;
    case GltfQuantizedFloatPolicy::SignedPositionDelta:
        return gltfSignedQuantizedFloatComponentType(componentType);
    case GltfQuantizedFloatPolicy::SignedNormalizedInteger:
        return normalized && gltfSignedQuantizedFloatComponentType(componentType);
    }
    return false;
}

const nlohmann::json* gltfAccessorJson(const nlohmann::json& doc, int accessorIndex) {
    if (!doc.contains("accessors") || !doc["accessors"].is_array() || accessorIndex < 0 ||
        static_cast<size_t>(accessorIndex) >= doc["accessors"].size()) {
        return nullptr;
    }
    return &doc["accessors"][static_cast<size_t>(accessorIndex)];
}

bool jsonStringArrayContains(const nlohmann::json& doc, const char* key, const char* value) {
    if (!doc.contains(key) || !doc[key].is_array()) {
        return false;
    }
    for (const nlohmann::json& item : doc[key]) {
        if (item.is_string() && item.get<std::string>() == value) {
            return true;
        }
    }
    return false;
}

nlohmann::json unsupportedRequiredGltfExtensionsForImportReport(const nlohmann::json& doc) {
    nlohmann::json unsupported = nlohmann::json::array();
    if (!doc.is_object() || !doc.contains("extensionsRequired") || !doc["extensionsRequired"].is_array()) {
        return unsupported;
    }
    for (const nlohmann::json& item : doc["extensionsRequired"]) {
        if (!item.is_string()) {
            continue;
        }
        const std::string extension = item.get<std::string>();
        if (!supportedGltfExtensionForImportReport(extension)) {
            unsupported.push_back(extension);
        }
    }
    return unsupported;
}

std::string unsupportedRequiredGltfExtensionMessage(const nlohmann::json& unsupported) {
    std::ostringstream out;
    out << "glTF import blocked: unsupported required extension" << (unsupported.size() == 1 ? "" : "s") << ": ";
    for (size_t i = 0; i < unsupported.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << unsupported[i].get<std::string>();
    }
    return out.str();
}

std::string mtlTexturePathToken(const std::vector<std::string>& tokens) {
    if (tokens.size() < 2) {
        return {};
    }
    for (size_t i = tokens.size(); i-- > 1;) {
        if (!tokens[i].empty() && tokens[i][0] != '-') {
            return tokens[i];
        }
    }
    return {};
}

bool isMtlTextureMapKey(const std::string& key) {
    const std::string normalized = lowerString(key);
    return normalized.rfind("map_", 0) == 0 || normalized == "bump" || normalized == "disp" || normalized == "decal";
}

std::string mtlTextureMapRoleLabel(const std::string& key) {
    const std::string normalized = lowerString(key);
    if (normalized == "map_kd") return "Base color texture";
    if (normalized == "map_ke") return "Emissive texture";
    if (normalized == "map_ks") return "Specular color texture";
    if (normalized == "map_ns") return "Specular exponent/roughness texture";
    if (normalized == "map_d") return "Opacity texture";
    if (normalized == "map_pr") return "Roughness texture";
    if (normalized == "map_pm") return "Metallic texture";
    if (normalized == "map_bump" || normalized == "bump") return "Normal/bump texture";
    if (normalized == "disp") return "Displacement texture";
    if (normalized == "decal") return "Stencil/decal texture";
    if (normalized.rfind("map_", 0) == 0) return "Texture map";
    return "Texture reference";
}

nlohmann::json mtlTextureMapOptionsJson(const std::vector<std::string>& tokens) {
    nlohmann::json options = nlohmann::json::object();
    nlohmann::json unsupported = nlohmann::json::array();
    auto parseVec = [&](size_t begin, size_t maxCount) {
        nlohmann::json values = nlohmann::json::array();
        for (size_t i = begin; i < tokens.size() && values.size() < maxCount; ++i) {
            if (!tokens[i].empty() && tokens[i][0] == '-') {
                break;
            }
            char* end = nullptr;
            const float parsed = std::strtof(tokens[i].c_str(), &end);
            if (end == tokens[i].c_str()) {
                break;
            }
            values.push_back(parsed);
        }
        return values;
    };
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        if (token.empty() || token[0] != '-') {
            continue;
        }
        if (token == "-s" || token == "-scale") {
            options["scale"] = parseVec(i + 1u, 3u);
        } else if (token == "-o" || token == "-offset") {
            options["offset"] = parseVec(i + 1u, 3u);
        } else if (token == "-bm") {
            options["bumpMultiplier"] = parseVec(i + 1u, 1u);
        } else if (token == "-clamp") {
            if (i + 1u < tokens.size()) {
                options["clamp"] = lowerString(tokens[i + 1u]) == "on";
            }
        } else if (token == "-type") {
            if (i + 1u < tokens.size()) {
                options["type"] = tokens[i + 1u];
            }
        } else if (token == "-blendu" || token == "-blendv" || token == "-cc" || token == "-mm" || token == "-t" || token == "-texres" || token == "-imfchan") {
            unsupported.push_back(token);
        }
    }
    if (!unsupported.empty()) {
        options["unsupportedOptions"] = unsupported;
    }
    return options;
}

nlohmann::json inspectMtlSource(const std::filesystem::path& sourcePath, std::vector<std::string>& warnings) {
    std::ifstream file(sourcePath);
    if (!file.is_open()) {
        warnings.push_back("Could not inspect MTL source metadata; file could not be opened.");
        return {{"inspected", false}, {"format", "MTL"}};
    }

    nlohmann::json materials = nlohmann::json::array();
    nlohmann::json textureReferences = nlohmann::json::array();
    std::unordered_set<std::string> uniqueTextureReferences;
    nlohmann::json currentMaterial;
    size_t propertyCount = 0;
    size_t textureMapCount = 0;

    auto flushMaterial = [&] {
        if (!currentMaterial.is_null() && currentMaterial.is_object()) {
            materials.push_back(currentMaterial);
        }
        currentMaterial = nlohmann::json::object();
    };

    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = trimAscii(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const std::vector<std::string> tokens = whitespaceTokens(trimmed);
        if (tokens.empty()) {
            continue;
        }
        const std::string key = tokens[0];
        if (key == "newmtl") {
            flushMaterial();
            currentMaterial["name"] = trimAscii(std::string_view(trimmed).substr(6));
            currentMaterial["properties"] = nlohmann::json::object();
            currentMaterial["textureMaps"] = nlohmann::json::object();
            currentMaterial["textureMapRoles"] = nlohmann::json::object();
            currentMaterial["textureMapOptions"] = nlohmann::json::object();
            continue;
        }
        if (currentMaterial.is_null() || !currentMaterial.is_object()) {
            currentMaterial = {
                {"name", "DefaultMaterial"},
                {"properties", nlohmann::json::object()},
                {"textureMaps", nlohmann::json::object()},
                {"textureMapRoles", nlohmann::json::object()},
                {"textureMapOptions", nlohmann::json::object()},
            };
        }
        if (isMtlTextureMapKey(key)) {
            const std::string texturePath = mtlTexturePathToken(tokens);
            currentMaterial["textureMaps"][key] = trimAscii(std::string_view(trimmed).substr(key.size()));
            currentMaterial["textureMapRoles"][key] = mtlTextureMapRoleLabel(key);
            currentMaterial["textureMapOptions"][key] = mtlTextureMapOptionsJson(tokens);
            if (!texturePath.empty() && uniqueTextureReferences.insert(texturePath).second) {
                textureReferences.push_back(texturePath);
            }
            ++textureMapCount;
        } else {
            currentMaterial["properties"][key] = floatTokenArray(tokens, 1);
            ++propertyCount;
        }
    }
    flushMaterial();

    return {
        {"inspected", true},
        {"format", "MTL"},
        {"materialCount", materials.size()},
        {"propertyCount", propertyCount},
        {"textureMapCount", textureMapCount},
        {"materials", materials},
        {"textureReferences", textureReferences},
        {"runtimeSupport", "metadata_only_native_material_cook_pending"},
    };
}

float jsonFloatAt(const nlohmann::json& values, size_t index, float fallback) {
    if (!values.is_array() || index >= values.size()) {
        return fallback;
    }
    const nlohmann::json& value = values[index];
    return value.is_number() ? value.get<float>() : fallback;
}

const nlohmann::json* jsonObjectValueCaseInsensitive(const nlohmann::json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const std::string target = lowerString(std::string(key));
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (lowerString(it.key()) == target) {
            return &it.value();
        }
    }
    return nullptr;
}

float mtlScalarProperty(const nlohmann::json& properties, std::string_view key, float fallback) {
    const nlohmann::json* values = jsonObjectValueCaseInsensitive(properties, key);
    return values != nullptr ? jsonFloatAt(*values, 0, fallback) : fallback;
}

glm::vec3 mtlVec3Property(const nlohmann::json& properties, std::string_view key, glm::vec3 fallback) {
    const nlohmann::json* values = jsonObjectValueCaseInsensitive(properties, key);
    if (values == nullptr) {
        return fallback;
    }
    return glm::vec3{
        jsonFloatAt(*values, 0, fallback.x),
        jsonFloatAt(*values, 1, fallback.y),
        jsonFloatAt(*values, 2, fallback.z),
    };
}

std::string mtlTexturePathValueToken(const std::string& value) {
    const std::vector<std::string> tokens = whitespaceTokens(value);
    for (size_t i = tokens.size(); i-- > 0;) {
        if (!tokens[i].empty() && tokens[i][0] != '-') {
            return tokens[i];
        }
    }
    return {};
}

float mtlTextureOptionFloat(const std::string& value, std::string_view option, float fallback) {
    const std::vector<std::string> tokens = whitespaceTokens(value);
    const std::string optionText(option);
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == optionText) {
            char* end = nullptr;
            const float parsed = std::strtof(tokens[i + 1].c_str(), &end);
            if (end != tokens[i + 1].c_str()) {
                return parsed;
            }
        }
    }
    return fallback;
}

nlohmann::json mtlTextureMapOptionsForKey(const nlohmann::json& textureMapOptions, std::string_view key) {
    const nlohmann::json* options = jsonObjectValueCaseInsensitive(textureMapOptions, key);
    return options != nullptr && options->is_object() ? *options : nlohmann::json::object();
}

nlohmann::json mtlTextureMapOptionRuntimeDiagnostics(const nlohmann::json& options, std::string_view key) {
    nlohmann::json diagnostics = {
        {"schema", "MtlTextureMapOptionRuntimeDiagnosticsV1"},
        {"mtlKey", std::string(key)},
        {"options", options},
        {"appliedOptions", nlohmann::json::array()},
        {"fallbackOptions", nlohmann::json::array()},
    };
    auto fallback = [&](const char* option, const char* reason) {
        if (options.contains(option)) {
            diagnostics["fallbackOptions"].push_back({{"option", option}, {"reason", reason}});
        }
    };
    if (options.contains("bumpMultiplier") && (lowerString(std::string(key)) == "disp")) {
        diagnostics["appliedOptions"].push_back({{"option", "bumpMultiplier"}, {"target", "MaterialAsset.heightScale"}});
    } else {
        fallback("bumpMultiplier", "bump_multiplier_preserved_but_not_applied_to_non_displacement_texture_slot");
    }
    fallback("scale", "texture_transform_preserved_but_runtime_material_texture_transform_not_implemented_for_mtl");
    fallback("offset", "texture_transform_preserved_but_runtime_material_texture_transform_not_implemented_for_mtl");
    fallback("clamp", "texture_clamp_mode_preserved_but_runtime_sampler_override_not_implemented_for_mtl");
    fallback("type", "texture_type_hint_preserved_but_runtime_slot_semantics_use_mtl_key_mapping");
    if (options.contains("unsupportedOptions")) {
        diagnostics["fallbackOptions"].push_back({
            {"option", "unsupportedOptions"},
            {"values", options["unsupportedOptions"]},
            {"reason", "mtl_texture_map_options_not_representable_in_current_material_schema"},
        });
    }
    return diagnostics;
}

NativeTextureRole mtlTextureRoleForKey(std::string_view key) {
    const std::string normalized = lowerString(std::string(key));
    if (normalized == "map_kd") return NativeTextureRole::BaseColor;
    if (normalized == "map_ke") return NativeTextureRole::Emissive;
    if (normalized == "map_bump" || normalized == "bump") return NativeTextureRole::Normal;
    if (normalized == "map_pr" || normalized == "map_ns") return NativeTextureRole::Roughness;
    if (normalized == "map_pm") return NativeTextureRole::Metallic;
    if (normalized == "map_d") return NativeTextureRole::Opacity;
    if (normalized == "disp") return NativeTextureRole::Height;
    if (normalized == "map_ks") return NativeTextureRole::Data;
    return NativeTextureRole::Unknown;
}

NativeTextureColorSpace mtlTextureColorSpaceForRole(NativeTextureRole role) {
    return (role == NativeTextureRole::BaseColor || role == NativeTextureRole::Emissive)
        ? NativeTextureColorSpace::Srgb
        : NativeTextureColorSpace::Linear;
}

std::string mtlTextureRoleStringForKey(std::string_view key) {
    return nativeTextureRoleName(mtlTextureRoleForKey(key));
}

MaterialAsset mtlMaterialAssetFromMetadata(const nlohmann::json& materialJson, std::string fallbackName) {
    MaterialAsset material;
    material.name = materialJson.value("name", std::move(fallbackName));
    const nlohmann::json emptyProperties = nlohmann::json::object();
    const nlohmann::json& properties = materialJson.contains("properties") ? materialJson["properties"] : emptyProperties;
    material.baseColorFactor = glm::vec4(mtlVec3Property(properties, "Kd", glm::vec3{1.0f}), 1.0f);
    material.emissiveFactor = mtlVec3Property(properties, "Ke", glm::vec3{0.0f});
    const glm::vec3 specular = mtlVec3Property(properties, "Ks", glm::vec3{0.0f});
    if (glm::dot(specular, specular) > 1.0e-8f) {
        material.hasSpecular = 1u;
        material.specularColorFactor = specular;
        material.specularFactor = std::clamp((specular.x + specular.y + specular.z) / 3.0f, 0.0f, 1.0f);
    }
    const float ns = mtlScalarProperty(properties, "Ns", -1.0f);
    if (ns >= 0.0f) {
        material.roughnessFactor = std::clamp(std::sqrt(2.0f / (std::max(ns, 0.0f) + 2.0f)), 0.04f, 1.0f);
    }
    material.roughnessFactor = std::clamp(mtlScalarProperty(properties, "Pr", material.roughnessFactor), 0.04f, 1.0f);
    material.metallicFactor = std::clamp(mtlScalarProperty(properties, "Pm", material.metallicFactor), 0.0f, 1.0f);
    const float ior = mtlScalarProperty(properties, "Ni", -1.0f);
    if (ior > 0.0f) {
        material.hasIor = 1u;
        material.iorFactor = std::clamp(ior, 1.0f, 3.0f);
    }
    const float illum = mtlScalarProperty(properties, "illum", -1.0f);
    float alpha = mtlScalarProperty(properties, "d", 1.0f);
    if (jsonObjectValueCaseInsensitive(properties, "Tr") != nullptr) {
        alpha = 1.0f - mtlScalarProperty(properties, "Tr", 0.0f);
    }
    material.baseColorFactor.w = std::clamp(alpha, 0.0f, 1.0f);
    if (material.baseColorFactor.w < 0.999f) {
        material.alphaMode = kMaterialAlphaModeBlend;
    }
    if (illum == 4.0f || illum == 6.0f || illum == 7.0f || illum == 9.0f || material.baseColorFactor.w < 0.999f) {
        material.hasTransmission = 1u;
        material.transmissionFactor = std::clamp(1.0f - material.baseColorFactor.w, 0.0f, 1.0f);
        if (material.transmissionFactor <= 0.0f && illum >= 4.0f) {
            material.transmissionFactor = 0.25f;
        }
    }
    return material;
}

void copyMtlTextureReferences(
    const std::filesystem::path& originalSource,
    const std::filesystem::path& copiedSource,
    const nlohmann::json& mtlMetadata,
    const std::filesystem::path& allowedRoot,
    std::vector<std::filesystem::path>& generatedFiles,
    std::vector<std::string>& warnings,
    std::vector<std::string>& errors,
    std::unordered_set<std::string>& copiedKeys) {
    if (!mtlMetadata.contains("textureReferences") || !mtlMetadata["textureReferences"].is_array()) {
        return;
    }
    for (const nlohmann::json& item : mtlMetadata["textureReferences"]) {
        if (!item.is_string()) {
            continue;
        }
        const std::string texture = item.get<std::string>();
        const std::filesystem::path relativePath = std::filesystem::path(texture).lexically_normal();
        if (relativePath.is_absolute()) {
            warnings.push_back("Skipped absolute MTL texture reference while copying source assets: " + texture);
            continue;
        }
        const std::filesystem::path sourcePath = (originalSource.parent_path() / relativePath).lexically_normal();
        const std::filesystem::path destinationPath = (copiedSource.parent_path() / relativePath).lexically_normal();
        if (!pathIsWithinLexicalRoot(destinationPath, allowedRoot)) {
            warnings.push_back("Skipped MTL texture reference outside SourceAssets import folder: " + texture);
            continue;
        }
        if (!copyImportSourceFile(sourcePath, destinationPath, generatedFiles, errors, copiedKeys)) {
            return;
        }
    }
}

nlohmann::json inspectObjSource(const std::filesystem::path& sourcePath, std::vector<std::string>& warnings) {
    nlohmann::json materials = nlohmann::json::array();
    nlohmann::json materialLibraries = nlohmann::json::array();
    nlohmann::json collisionCandidates = nlohmann::json::array();
    nlohmann::json lodCandidates = nlohmann::json::array();
    std::unordered_set<std::string> uniqueMaterials;
    std::unordered_set<std::string> uniqueLibraries;
    std::unordered_set<std::string> uniqueCollisionCandidates;
    std::unordered_set<std::string> uniqueLodCandidates;
    size_t vertexCount = 0;
    size_t texcoordCount = 0;
    size_t normalCount = 0;
    size_t faceCount = 0;
    size_t triangleFaceCount = 0;
    size_t quadFaceCount = 0;
    size_t ngonFaceCount = 0;
    size_t objectCount = 0;
    size_t groupCount = 0;

    std::ifstream file(sourcePath);
    if (!file.is_open()) {
        warnings.push_back("Could not inspect OBJ source metadata; file could not be opened.");
        return {{"inspected", false}, {"format", "OBJ"}};
    }

    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = trimAscii(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        if (trimmed.rfind("v ", 0) == 0) {
            ++vertexCount;
        } else if (trimmed.rfind("vt ", 0) == 0) {
            ++texcoordCount;
        } else if (trimmed.rfind("vn ", 0) == 0) {
            ++normalCount;
        } else if (trimmed.rfind("f ", 0) == 0) {
            ++faceCount;
            std::istringstream faceStream(trimAscii(std::string_view(trimmed).substr(2)));
            size_t faceVertexCount = 0;
            std::string token;
            while (faceStream >> token) {
                ++faceVertexCount;
            }
            if (faceVertexCount == 3u) {
                ++triangleFaceCount;
            } else if (faceVertexCount == 4u) {
                ++quadFaceCount;
            } else if (faceVertexCount > 4u) {
                ++ngonFaceCount;
            }
        } else if (trimmed.rfind("o ", 0) == 0) {
            ++objectCount;
            appendCollisionLodNameMetadata(
                firstObjToken(std::string_view(trimmed).substr(2)),
                "object",
                objectCount - 1,
                collisionCandidates,
                lodCandidates,
                uniqueCollisionCandidates,
                uniqueLodCandidates);
        } else if (trimmed.rfind("g ", 0) == 0) {
            ++groupCount;
            appendCollisionLodNameMetadata(
                firstObjToken(std::string_view(trimmed).substr(2)),
                "group",
                groupCount - 1,
                collisionCandidates,
                lodCandidates,
                uniqueCollisionCandidates,
                uniqueLodCandidates);
        } else if (trimmed.rfind("usemtl ", 0) == 0) {
            const std::string material = firstObjToken(std::string_view(trimmed).substr(7));
            if (!material.empty() && uniqueMaterials.insert(material).second) {
                materials.push_back(material);
            }
        } else if (trimmed.rfind("mtllib ", 0) == 0) {
            std::string rest = trimAscii(std::string_view(trimmed).substr(7));
            while (!rest.empty()) {
                const std::string library = firstObjToken(rest);
                if (library.empty()) {
                    break;
                }
                if (uniqueLibraries.insert(library).second) {
                    materialLibraries.push_back(library);
                }
                rest = trimAscii(std::string_view(rest).substr(std::min(library.size(), rest.size())));
            }
        }
    }

    if (!collisionCandidates.empty()) {
        addWarningOnce(warnings, "Source contains collision-named OBJ objects/groups; collision metadata was preserved for downstream physics/runtime tooling.");
    }
    if (!lodCandidates.empty()) {
        addWarningOnce(warnings, "Source contains LOD-named OBJ objects/groups; LOD metadata was preserved for downstream renderer/runtime tooling.");
    }

    return {
        {"inspected", true},
        {"format", "OBJ"},
        {"vertexCount", vertexCount},
        {"texcoordCount", texcoordCount},
        {"normalCount", normalCount},
        {"faceCount", faceCount},
        {"triangleFaceCount", triangleFaceCount},
        {"quadFaceCount", quadFaceCount},
        {"ngonFaceCount", ngonFaceCount},
        {"triangulation", quadFaceCount > 0 || ngonFaceCount > 0 ? "tinyobjloader_deterministic_fan_enabled_for_runtime_cook" : "source_already_triangular_or_lines_ignored"},
        {"objectCount", objectCount},
        {"groupCount", groupCount},
        {"materials", materials},
        {"materialLibraries", materialLibraries},
        {"collisionLodMetadata", collisionLodMetadataJson("OBJ", true, collisionCandidates, lodCandidates)},
        {"runtimeSupport", "metadata_only_native_mesh_cook_pending"},
    };
}

struct ObjRuntimeMeshCookData {
    bool supported = false;
    MeshAsset mesh;
    nlohmann::json diagnostics = nlohmann::json::object();
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

struct ObjGeneratedNormalRef {
    uint32_t vertexIndex = 0;
    int sourceVertexIndex = -1;
    unsigned int smoothingGroup = 0;
    glm::vec3 faceNormal{0.0f, 1.0f, 0.0f};
};

ObjRuntimeMeshCookData loadObjRuntimeMesh(const std::filesystem::path& sourcePath, std::string_view displayName) {
    ObjRuntimeMeshCookData out;
#if RTV_ENABLE_TINYOBJ_IMPORTER && RTV_TINYOBJ_IMPORTER_AVAILABLE
    out.supported = true;
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.mtl_search_path = sourcePath.parent_path().string();

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(sourcePath.string(), config)) {
        if (!reader.Error().empty()) {
            out.errors.push_back(reader.Error());
        } else {
            out.errors.push_back("tinyobjloader failed to parse OBJ source.");
        }
        return out;
    }
    if (!reader.Warning().empty()) {
        out.warnings.push_back(reader.Warning());
    }

    const tinyobj::attrib_t& attrib = reader.GetAttrib();
    const std::vector<tinyobj::shape_t>& shapes = reader.GetShapes();
    const std::vector<tinyobj::material_t>& materials = reader.GetMaterials();

    out.mesh.name = std::string(displayName.empty() ? sourcePath.stem().string() : std::string(displayName));
    nlohmann::json shapeReports = nlohmann::json::array();
    std::unordered_set<int> usedMaterialIds;
    std::unordered_set<unsigned int> smoothingGroupIds;
    size_t skippedFaces = 0;
    bool generatedAnyNormals = false;
    bool generatedSmoothingAwareNormals = false;
    bool decodedAnyVertexColors = false;
    bool generatedAnyTangents = false;
    // Index diagnostics. tinyobjloader resolves negative/relative OBJ indices to absolute
    // indices before we read them, so genuine negative-index detection requires a raw source
    // scan (done below). Mixed-indexing and out-of-range counters are derived from the resolved
    // index records during triangulation.
    size_t mixedIndexFaceCount = 0;        // faces whose corners disagree on texcoord/normal presence
    size_t outOfRangePositionIndexCount = 0;
    size_t outOfRangeTexcoordIndexCount = 0;
    size_t outOfRangeNormalIndexCount = 0;
    std::vector<ObjGeneratedNormalRef> generatedNormalRefs;

    auto appendPrimitive = [&](uint32_t firstVertex, uint32_t firstIndex, int materialId, const std::string& label) {
        const uint32_t indexCount = static_cast<uint32_t>(out.mesh.indices.size()) - firstIndex;
        const uint32_t vertexCount = static_cast<uint32_t>(out.mesh.vertices.size()) - firstVertex;
        if (indexCount == 0 || vertexCount == 0) {
            return;
        }
        MeshPrimitiveAsset primitive;
        primitive.firstVertex = firstVertex;
        primitive.vertexCount = vertexCount;
        primitive.firstIndex = firstIndex;
        primitive.indexCount = indexCount;
        if (materialId >= 0 && static_cast<size_t>(materialId) < materials.size()) {
            primitive.material = MaterialAssetHandle{static_cast<uint32_t>(materialId)};
        }
        primitive.alphaMode = kMaterialAlphaModeOpaque;
        primitive.alphaCutoff = 0.5f;
        out.mesh.primitives.push_back(primitive);
        shapeReports.push_back({
            {"label", label},
            {"materialId", materialId},
            {"firstVertex", firstVertex},
            {"vertexCount", vertexCount},
            {"firstIndex", firstIndex},
            {"indexCount", indexCount},
        });
    };

    for (const tinyobj::shape_t& shape : shapes) {
        size_t indexOffset = 0;
        uint32_t primitiveFirstVertex = static_cast<uint32_t>(out.mesh.vertices.size());
        uint32_t primitiveFirstIndex = static_cast<uint32_t>(out.mesh.indices.size());
        int primitiveMaterialId = std::numeric_limits<int>::min();
        const std::string shapeLabel = shape.name.empty() ? "OBJShape" : shape.name;

        for (size_t faceIndex = 0; faceIndex < shape.mesh.num_face_vertices.size(); ++faceIndex) {
            const size_t faceVertexCount = static_cast<size_t>(shape.mesh.num_face_vertices[faceIndex]);
            const int materialId = faceIndex < shape.mesh.material_ids.size() ? shape.mesh.material_ids[faceIndex] : -1;
            const unsigned int smoothingGroup = faceIndex < shape.mesh.smoothing_group_ids.size() ? shape.mesh.smoothing_group_ids[faceIndex] : 0u;
            if (faceVertexCount != 3) {
                indexOffset += faceVertexCount;
                ++skippedFaces;
                continue;
            }
            if (primitiveMaterialId == std::numeric_limits<int>::min()) {
                primitiveMaterialId = materialId;
            } else if (materialId != primitiveMaterialId) {
                appendPrimitive(primitiveFirstVertex, primitiveFirstIndex, primitiveMaterialId, shapeLabel);
                primitiveFirstVertex = static_cast<uint32_t>(out.mesh.vertices.size());
                primitiveFirstIndex = static_cast<uint32_t>(out.mesh.indices.size());
                primitiveMaterialId = materialId;
            }
            if (materialId >= 0) {
                usedMaterialIds.insert(materialId);
            }

            const size_t triangleVertexBase = out.mesh.vertices.size();
            bool triangleMissingNormal = false;
            bool triangleHasUv = false;
            std::array<int, 3> triangleSourceVertexIndices{-1, -1, -1};
            // Per-corner attribute presence, used to detect mixed indexing within one face
            // (e.g. "f a/1 b c/3" where only some corners carry texcoords/normals).
            std::array<bool, 3> cornerHasTexcoord{false, false, false};
            std::array<bool, 3> cornerHasNormal{false, false, false};
            for (size_t vertexInFace = 0; vertexInFace < 3; ++vertexInFace) {
                const tinyobj::index_t index = shape.mesh.indices[indexOffset + vertexInFace];
                MeshVertex vertex;
                if (index.vertex_index >= 0) {
                    triangleSourceVertexIndices[vertexInFace] = index.vertex_index;
                    const size_t source = static_cast<size_t>(index.vertex_index) * 3u;
                    if (source + 2u < attrib.vertices.size()) {
                        vertex.position = glm::vec3{attrib.vertices[source], attrib.vertices[source + 1u], attrib.vertices[source + 2u]};
                    } else {
                        ++outOfRangePositionIndexCount;
                    }
                    if (source + 2u < attrib.colors.size()) {
                        vertex.color = glm::vec4{attrib.colors[source], attrib.colors[source + 1u], attrib.colors[source + 2u], 1.0f};
                        decodedAnyVertexColors = true;
                    }
                }
                if (index.normal_index >= 0) {
                    cornerHasNormal[vertexInFace] = true;
                    const size_t source = static_cast<size_t>(index.normal_index) * 3u;
                    if (source + 2u < attrib.normals.size()) {
                        const glm::vec3 sourceNormal{attrib.normals[source], attrib.normals[source + 1u], attrib.normals[source + 2u]};
                        const float normalLength2 = glm::dot(sourceNormal, sourceNormal);
                        if (normalLength2 > 1.0e-12f) {
                            vertex.normal = sourceNormal / std::sqrt(normalLength2);
                        } else {
                            triangleMissingNormal = true;
                        }
                    } else {
                        ++outOfRangeNormalIndexCount;
                        triangleMissingNormal = true;
                    }
                } else {
                    triangleMissingNormal = true;
                }
                if (index.texcoord_index >= 0) {
                    cornerHasTexcoord[vertexInFace] = true;
                    const size_t source = static_cast<size_t>(index.texcoord_index) * 2u;
                    if (source + 1u < attrib.texcoords.size()) {
                        vertex.texcoord = glm::vec2{attrib.texcoords[source], 1.0f - attrib.texcoords[source + 1u]};
                        triangleHasUv = true;
                    } else {
                        ++outOfRangeTexcoordIndexCount;
                    }
                }
                out.mesh.vertices.push_back(vertex);
                out.mesh.indices.push_back(static_cast<uint32_t>(out.mesh.vertices.size() - 1u));
            }
            if ((cornerHasTexcoord[0] != cornerHasTexcoord[1]) || (cornerHasTexcoord[1] != cornerHasTexcoord[2]) ||
                (cornerHasNormal[0] != cornerHasNormal[1]) || (cornerHasNormal[1] != cornerHasNormal[2])) {
                ++mixedIndexFaceCount;
            }
            if (triangleMissingNormal && triangleVertexBase + 2u < out.mesh.vertices.size()) {
                const glm::vec3 p0 = out.mesh.vertices[triangleVertexBase].position;
                const glm::vec3 p1 = out.mesh.vertices[triangleVertexBase + 1u].position;
                const glm::vec3 p2 = out.mesh.vertices[triangleVertexBase + 2u].position;
                const glm::vec3 cross = glm::cross(p1 - p0, p2 - p0);
                const float length2 = glm::dot(cross, cross);
                const glm::vec3 normal = length2 > 1.0e-12f ? cross / std::sqrt(length2) : glm::vec3{0.0f, 1.0f, 0.0f};
                out.mesh.vertices[triangleVertexBase].normal = normal;
                out.mesh.vertices[triangleVertexBase + 1u].normal = normal;
                out.mesh.vertices[triangleVertexBase + 2u].normal = normal;
                generatedAnyNormals = true;
                if (smoothingGroup != 0u) {
                    generatedSmoothingAwareNormals = true;
                    for (size_t vertexInFace = 0; vertexInFace < 3; ++vertexInFace) {
                        if (triangleSourceVertexIndices[vertexInFace] >= 0) {
                            generatedNormalRefs.push_back(ObjGeneratedNormalRef{
                                .vertexIndex = static_cast<uint32_t>(triangleVertexBase + vertexInFace),
                                .sourceVertexIndex = triangleSourceVertexIndices[vertexInFace],
                                .smoothingGroup = smoothingGroup,
                                .faceNormal = normal,
                            });
                        }
                    }
                }
            }
            if (triangleHasUv && triangleVertexBase + 2u < out.mesh.vertices.size()) {
                const glm::vec3 p0 = out.mesh.vertices[triangleVertexBase].position;
                const glm::vec3 p1 = out.mesh.vertices[triangleVertexBase + 1u].position;
                const glm::vec3 p2 = out.mesh.vertices[triangleVertexBase + 2u].position;
                const glm::vec2 uv0 = out.mesh.vertices[triangleVertexBase].texcoord;
                const glm::vec2 uv1 = out.mesh.vertices[triangleVertexBase + 1u].texcoord;
                const glm::vec2 uv2 = out.mesh.vertices[triangleVertexBase + 2u].texcoord;
                const glm::vec3 edge1 = p1 - p0;
                const glm::vec3 edge2 = p2 - p0;
                const glm::vec2 duv1 = uv1 - uv0;
                const glm::vec2 duv2 = uv2 - uv0;
                const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
                if (std::abs(determinant) > 1.0e-8f) {
                    const float invDeterminant = 1.0f / determinant;
                    const glm::vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) * invDeterminant;
                    const float tangentLength2 = glm::dot(tangent, tangent);
                    if (tangentLength2 > 1.0e-12f) {
                        const glm::vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * invDeterminant;
                        const glm::vec3 normal = out.mesh.vertices[triangleVertexBase].normal;
                        const float sign = glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
                        const glm::vec4 encodedTangent{tangent / std::sqrt(tangentLength2), sign};
                        out.mesh.vertices[triangleVertexBase].tangent = encodedTangent;
                        out.mesh.vertices[triangleVertexBase + 1u].tangent = encodedTangent;
                        out.mesh.vertices[triangleVertexBase + 2u].tangent = encodedTangent;
                        generatedAnyTangents = true;
                    }
                }
            }
            if (smoothingGroup != 0u) {
                smoothingGroupIds.insert(smoothingGroup);
            }
            indexOffset += faceVertexCount;
        }
        if (primitiveMaterialId != std::numeric_limits<int>::min()) {
            appendPrimitive(primitiveFirstVertex, primitiveFirstIndex, primitiveMaterialId, shapeLabel);
        }
    }

    if (!generatedNormalRefs.empty()) {
        std::unordered_map<uint64_t, glm::vec3> smoothedNormalSums;
        smoothedNormalSums.reserve(generatedNormalRefs.size());
        for (const ObjGeneratedNormalRef& ref : generatedNormalRefs) {
            const uint64_t key = (static_cast<uint64_t>(ref.smoothingGroup) << 32ull) |
                                 static_cast<uint32_t>(ref.sourceVertexIndex);
            auto [it, inserted] = smoothedNormalSums.emplace(key, glm::vec3{0.0f});
            it->second += ref.faceNormal;
        }
        for (const ObjGeneratedNormalRef& ref : generatedNormalRefs) {
            const uint64_t key = (static_cast<uint64_t>(ref.smoothingGroup) << 32ull) |
                                 static_cast<uint32_t>(ref.sourceVertexIndex);
            const auto found = smoothedNormalSums.find(key);
            if (found == smoothedNormalSums.end()) {
                continue;
            }
            const float length2 = glm::dot(found->second, found->second);
            if (length2 > 1.0e-12f && ref.vertexIndex < out.mesh.vertices.size()) {
                out.mesh.vertices[ref.vertexIndex].normal = found->second / std::sqrt(length2);
            }
        }
    }

    if (out.mesh.vertices.empty() || out.mesh.indices.empty() || out.mesh.primitives.empty()) {
        out.errors.push_back("OBJ source did not contain renderable triangulated geometry.");
    }
    if (skippedFaces > 0) {
        out.warnings.push_back("Skipped " + std::to_string(skippedFaces) + " non-triangulated OBJ faces after tinyobjloader parse.");
    }
    if (generatedAnyNormals) {
        out.warnings.push_back("Generated face normals for OBJ triangles missing normal attributes.");
    }
    if (generatedSmoothingAwareNormals) {
        out.warnings.push_back("Averaged generated OBJ normals by source vertex and smoothing group.");
    }

    // Raw source scan for genuine negative/relative OBJ indices. tinyobjloader resolves these
    // to absolute indices before we read shape.mesh.indices, so the only way to report that the
    // source authored relative indexing is to scan the original 'f' lines. This is diagnostic
    // only; the resolved geometry above is already correct.
    size_t negativeIndexReferenceCount = 0;
    size_t negativeIndexFaceCount = 0;
    {
        std::ifstream objFile(sourcePath);
        if (objFile) {
            std::string line;
            while (std::getline(objFile, line)) {
                size_t pos = line.find_first_not_of(" \t");
                if (pos == std::string::npos || line[pos] != 'f') {
                    continue;
                }
                // Require 'f' followed by whitespace to avoid matching tokens that merely start with f.
                if (pos + 1u >= line.size() || (line[pos + 1u] != ' ' && line[pos + 1u] != '\t')) {
                    continue;
                }
                bool faceHasNegative = false;
                std::istringstream stream(line.substr(pos + 1u));
                std::string token;
                while (stream >> token) {
                    // Each token is v[/vt][/vn]; a leading '-' on any component is a relative index.
                    size_t start = 0;
                    while (start < token.size()) {
                        size_t slash = token.find('/', start);
                        const std::string component = token.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                        if (!component.empty() && component[0] == '-') {
                            ++negativeIndexReferenceCount;
                            faceHasNegative = true;
                        }
                        if (slash == std::string::npos) {
                            break;
                        }
                        start = slash + 1u;
                    }
                }
                if (faceHasNegative) {
                    ++negativeIndexFaceCount;
                }
            }
        }
    }
    if (negativeIndexReferenceCount > 0) {
        out.warnings.push_back("OBJ source authored " + std::to_string(negativeIndexReferenceCount) +
            " relative/negative vertex index references across " + std::to_string(negativeIndexFaceCount) +
            " faces; tinyobjloader resolved them to absolute indices before runtime cooking.");
    }
    if (mixedIndexFaceCount > 0) {
        out.warnings.push_back("OBJ source contains " + std::to_string(mixedIndexFaceCount) +
            " faces with mixed per-corner indexing (corners disagree on texcoord/normal presence); missing attributes were generated where possible.");
    }
    const size_t outOfRangeIndexCount = outOfRangePositionIndexCount + outOfRangeTexcoordIndexCount + outOfRangeNormalIndexCount;
    if (outOfRangeIndexCount > 0) {
        out.warnings.push_back("OBJ source referenced " + std::to_string(outOfRangeIndexCount) +
            " out-of-range attribute indices (position/texcoord/normal); affected attributes fell back to defaults.");
    }

    nlohmann::json materialReports = nlohmann::json::array();
    for (size_t i = 0; i < materials.size(); ++i) {
        materialReports.push_back({
            {"id", i},
            {"name", materials[i].name},
            {"used", usedMaterialIds.contains(static_cast<int>(i))},
        });
    }
    out.diagnostics = {
        {"schema", "ObjRuntimeMeshCookDiagnosticsV1"},
        {"parser", "tinyobjloader"},
        {"triangulation", "tinyobjloader_deterministic_fan"},
        {"triangulateConfigEnabled", config.triangulate},
        {"shapeCount", shapes.size()},
        {"materialCount", materials.size()},
        {"usedMaterialCount", usedMaterialIds.size()},
        {"vertexCount", out.mesh.vertices.size()},
        {"indexCount", out.mesh.indices.size()},
        {"primitiveCount", out.mesh.primitives.size()},
        {"generatedNormals", generatedAnyNormals},
        {"generatedSmoothingAwareNormals", generatedSmoothingAwareNormals},
        {"generatedTangents", generatedAnyTangents},
        {"vertexColorsDecoded", decodedAnyVertexColors},
        {"smoothingGroupCount", smoothingGroupIds.size()},
        {"skippedFaceCount", skippedFaces},
        {"indexDiagnostics", {
            {"negativeIndexReferenceCount", negativeIndexReferenceCount},
            {"negativeIndexFaceCount", negativeIndexFaceCount},
            {"negativeIndicesResolvedBy", "tinyobjloader_absolute_resolution"},
            {"mixedIndexFaceCount", mixedIndexFaceCount},
            {"outOfRangePositionIndexCount", outOfRangePositionIndexCount},
            {"outOfRangeTexcoordIndexCount", outOfRangeTexcoordIndexCount},
            {"outOfRangeNormalIndexCount", outOfRangeNormalIndexCount},
            {"outOfRangeIndexCount", outOfRangeIndexCount},
        }},
        {"shapes", shapeReports},
        {"materials", materialReports},
    };
#else
    (void)sourcePath;
    (void)displayName;
    out.supported = false;
    out.errors.push_back("OBJ runtime mesh cooking requires RTV_ENABLE_TINYOBJ_IMPORTER=ON and tinyobjloader availability.");
    out.diagnostics = {
        {"schema", "ObjRuntimeMeshCookDiagnosticsV1"},
        {"parser", "tinyobjloader"},
        {"supported", false},
        {"disabledReason", "RTV_ENABLE_TINYOBJ_IMPORTER=OFF or tinyobjloader unavailable"},
    };
#endif
    return out;
}

struct FbxStaticImportData {
    bool supported = false;
    AssetManager assets;
    SceneAsset scene;
    std::vector<std::string> textureRoles;
    nlohmann::json skeletons = nlohmann::json::array();
    nlohmann::json animations = nlohmann::json::array();
    nlohmann::json diagnostics = nlohmann::json::object();
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

std::unordered_map<std::string, uint32_t> fbxJointIndexByName(const nlohmann::json& skeleton) {
    std::unordered_map<std::string, uint32_t> out;
    if (!skeleton.contains("joints") || !skeleton["joints"].is_array()) {
        return out;
    }
    for (const nlohmann::json& joint : skeleton["joints"]) {
        const std::string name = joint.value("name", std::string{});
        if (name.empty()) {
            continue;
        }
        out.emplace(name, joint.value("index", static_cast<uint32_t>(out.size())));
    }
    return out;
}

#if RTV_ENABLE_ASSIMP_IMPORTER && RTV_ASSIMP_IMPORTER_AVAILABLE
glm::mat4 assimpMatrixToGlm(const aiMatrix4x4& matrix) {
    return glm::mat4{
        matrix.a1, matrix.b1, matrix.c1, matrix.d1,
        matrix.a2, matrix.b2, matrix.c2, matrix.d2,
        matrix.a3, matrix.b3, matrix.c3, matrix.d3,
        matrix.a4, matrix.b4, matrix.c4, matrix.d4,
    };
}

glm::vec3 assimpColor3(const aiColor3D& color) {
    return glm::vec3{color.r, color.g, color.b};
}

std::string assimpString(const aiString& value) {
    return std::string(value.C_Str());
}

nlohmann::json assimpMetadataEntryJson(const aiMetadataEntry& entry) {
    if (entry.mData == nullptr) {
        return nullptr;
    }
    switch (entry.mType) {
    case AI_BOOL:
        return *static_cast<const bool*>(entry.mData);
    case AI_INT32:
        return *static_cast<const int32_t*>(entry.mData);
    case AI_UINT32:
        return *static_cast<const uint32_t*>(entry.mData);
    case AI_INT64:
        return *static_cast<const int64_t*>(entry.mData);
    case AI_UINT64:
        return *static_cast<const uint64_t*>(entry.mData);
    case AI_FLOAT:
        return *static_cast<const float*>(entry.mData);
    case AI_DOUBLE:
        return *static_cast<const double*>(entry.mData);
    case AI_AISTRING:
        return assimpString(*static_cast<const aiString*>(entry.mData));
    case AI_AIVECTOR3D: {
        const aiVector3D& value = *static_cast<const aiVector3D*>(entry.mData);
        return nlohmann::json::array({value.x, value.y, value.z});
    }
    case AI_AIMETADATA:
    case AI_META_MAX:
    default:
        return nullptr;
    }
}

const char* assimpMetadataTypeName(aiMetadataType type) {
    switch (type) {
    case AI_BOOL: return "bool";
    case AI_INT32: return "int32";
    case AI_UINT32: return "uint32";
    case AI_INT64: return "int64";
    case AI_UINT64: return "uint64";
    case AI_FLOAT: return "float";
    case AI_DOUBLE: return "double";
    case AI_AISTRING: return "string";
    case AI_AIVECTOR3D: return "vector3";
    case AI_AIMETADATA: return "metadata";
    case AI_META_MAX:
    default: return "unknown";
    }
}

std::string normalizeMetadataKey(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    return out;
}

bool metadataKeyMatches(const std::string& normalizedKey, std::initializer_list<const char*> aliases) {
    for (const char* alias : aliases) {
        if (normalizedKey == normalizeMetadataKey(alias)) {
            return true;
        }
    }
    return false;
}

std::optional<bool> assimpMetadataBoolValue(const aiMetadataEntry& entry) {
    if (entry.mData == nullptr) {
        return std::nullopt;
    }
    switch (entry.mType) {
    case AI_BOOL:
        return *static_cast<const bool*>(entry.mData);
    case AI_INT32:
        return *static_cast<const int32_t*>(entry.mData) != 0;
    case AI_UINT32:
        return *static_cast<const uint32_t*>(entry.mData) != 0u;
    case AI_INT64:
        return *static_cast<const int64_t*>(entry.mData) != 0;
    case AI_UINT64:
        return *static_cast<const uint64_t*>(entry.mData) != 0u;
    case AI_FLOAT:
        return std::abs(*static_cast<const float*>(entry.mData)) > 1.0e-7f;
    case AI_DOUBLE:
        return std::abs(*static_cast<const double*>(entry.mData)) > 1.0e-12;
    case AI_AISTRING: {
        const std::string value = lowerString(assimpString(*static_cast<const aiString*>(entry.mData)));
        if (value == "true" || value == "yes" || value == "on" || value == "visible" || value == "enabled" || value == "1") {
            return true;
        }
        if (value == "false" || value == "no" || value == "off" || value == "hidden" || value == "disabled" || value == "0") {
            return false;
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

std::optional<int32_t> assimpMetadataIntValue(const aiMetadataEntry& entry) {
    if (entry.mData == nullptr) {
        return std::nullopt;
    }
    switch (entry.mType) {
    case AI_INT32:
        return *static_cast<const int32_t*>(entry.mData);
    case AI_UINT32:
        return static_cast<int32_t>(*static_cast<const uint32_t*>(entry.mData));
    case AI_INT64:
        return static_cast<int32_t>(*static_cast<const int64_t*>(entry.mData));
    case AI_UINT64:
        return static_cast<int32_t>(*static_cast<const uint64_t*>(entry.mData));
    case AI_FLOAT:
        return static_cast<int32_t>(std::lround(*static_cast<const float*>(entry.mData)));
    case AI_DOUBLE:
        return static_cast<int32_t>(std::lround(*static_cast<const double*>(entry.mData)));
    default:
        return std::nullopt;
    }
}

nlohmann::json applyFbxNodeMetadataToSceneNode(const aiNode* node, SceneNodeAsset& sceneNode) {
    nlohmann::json report = {
        {"metadataCount", 0u},
        {"raw", nlohmann::json::array()},
        {"mapped", nlohmann::json::object()},
    };
    if (node == nullptr || node->mMetaData == nullptr) {
        return report;
    }
    const aiMetadata& metadata = *node->mMetaData;
    report["metadataCount"] = metadata.mNumProperties;
    for (unsigned i = 0; i < metadata.mNumProperties; ++i) {
        const aiString& key = metadata.mKeys[i];
        const aiMetadataEntry& entry = metadata.mValues[i];
        const std::string keyString = assimpString(key);
        const std::string normalizedKey = normalizeMetadataKey(keyString);
        const nlohmann::json valueJson = assimpMetadataEntryJson(entry);
        report["raw"].push_back({
            {"key", keyString},
            {"normalizedKey", normalizedKey},
            {"type", assimpMetadataTypeName(entry.mType)},
            {"value", valueJson},
        });

        if (metadataKeyMatches(normalizedKey, {"visibility", "visible", "show", "enabled", "display", "displayable"})) {
            if (const std::optional<bool> value = assimpMetadataBoolValue(entry)) {
                sceneNode.visible = *value;
                report["mapped"]["visible"] = *value;
            }
        } else if (metadataKeyMatches(normalizedKey, {"hidden", "hide"})) {
            if (const std::optional<bool> value = assimpMetadataBoolValue(entry)) {
                sceneNode.visible = !*value;
                report["mapped"]["visible"] = !*value;
                report["mapped"]["hiddenSource"] = *value;
            }
        } else if (metadataKeyMatches(normalizedKey, {"visibleToCamera", "cameraVisibility", "primaryVisibility", "visibleInCamera", "cameraVisible"})) {
            if (const std::optional<bool> value = assimpMetadataBoolValue(entry)) {
                sceneNode.visibleToCamera = *value;
                report["mapped"]["visibleToCamera"] = *value;
            }
        } else if (metadataKeyMatches(normalizedKey, {"castShadow", "castsShadow", "castsShadows", "shadowCaster"})) {
            if (const std::optional<bool> value = assimpMetadataBoolValue(entry)) {
                sceneNode.castShadow = *value;
                report["mapped"]["castShadow"] = *value;
            }
        } else if (metadataKeyMatches(normalizedKey, {"receiveShadow", "receivesShadow", "receivesShadows", "shadowReceiver"})) {
            if (const std::optional<bool> value = assimpMetadataBoolValue(entry)) {
                sceneNode.receiveShadow = *value;
                report["mapped"]["receiveShadow"] = *value;
            }
        } else if (metadataKeyMatches(normalizedKey, {"renderLayer", "renderLayerId", "layer", "layerId"})) {
            if (const std::optional<int32_t> value = assimpMetadataIntValue(entry)) {
                sceneNode.renderLayer = *value;
                report["mapped"]["renderLayer"] = *value;
            }
        }
    }
    return report;
}

const char* assimpTextureTypeName(aiTextureType type) {
    switch (type) {
    case aiTextureType_DIFFUSE: return "DIFFUSE";
    case aiTextureType_SPECULAR: return "SPECULAR";
    case aiTextureType_AMBIENT: return "AMBIENT";
    case aiTextureType_EMISSIVE: return "EMISSIVE";
    case aiTextureType_HEIGHT: return "HEIGHT";
    case aiTextureType_NORMALS: return "NORMALS";
    case aiTextureType_OPACITY: return "OPACITY";
    case aiTextureType_LIGHTMAP: return "LIGHTMAP";
    case aiTextureType_BASE_COLOR: return "BASE_COLOR";
    case aiTextureType_NORMAL_CAMERA: return "NORMAL_CAMERA";
    case aiTextureType_EMISSION_COLOR: return "EMISSION_COLOR";
    case aiTextureType_METALNESS: return "METALNESS";
    case aiTextureType_DIFFUSE_ROUGHNESS: return "DIFFUSE_ROUGHNESS";
    case aiTextureType_AMBIENT_OCCLUSION: return "AMBIENT_OCCLUSION";
    case aiTextureType_UNKNOWN: return "UNKNOWN";
    default: return "OTHER";
    }
}

bool fbxMaterialVertexColorsAreBlendMasks(std::string_view materialName) {
    const std::string name = lowerString(std::string(materialName));
    return name.find("blendshader") != std::string::npos || name.find("blend_shader") != std::string::npos;
}

NativeTextureColorSpace fbxTextureColorSpaceForRole(NativeTextureRole role) {
    return (role == NativeTextureRole::BaseColor || role == NativeTextureRole::Emissive)
        ? NativeTextureColorSpace::Srgb
        : NativeTextureColorSpace::Linear;
}

struct FbxTextureConvention {
    bool baseColorAlphaIsOpacity = false;
    bool specularIsPackedOcclusionRoughnessMetalness = false;
    bool normalMapIsDirectX = false;
    std::string source;
};

bool containsAnyLower(std::string_view text, std::initializer_list<const char*> needles) {
    const std::string lower = lowerString(std::string(text));
    for (const char* needle : needles) {
        if (lower.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

FbxTextureConvention detectFbxTextureConvention(const std::filesystem::path& sourcePath) {
    FbxTextureConvention convention;
    const std::filesystem::path readmePath = sourcePath.parent_path() / "README.txt";
    std::ifstream readme(readmePath);
    if (!readme.is_open()) {
        return convention;
    }
    std::stringstream buffer;
    buffer << readme.rdbuf();
    const std::string text = lowerString(buffer.str());
    const bool mentionsBaseAlphaOpacity = text.find("basecolor") != std::string::npos &&
        text.find("alpha channel") != std::string::npos &&
        text.find("opacity") != std::string::npos;
    const bool mentionsSpecularPackedOrm = text.find("specular") != std::string::npos &&
        text.find("red channel") != std::string::npos &&
        text.find("occlusion") != std::string::npos &&
        text.find("green channel") != std::string::npos &&
        text.find("roughness") != std::string::npos &&
        text.find("blue channel") != std::string::npos &&
        (text.find("metalness") != std::string::npos || text.find("metallic") != std::string::npos);
    const bool mentionsDirectXNormals = text.find("normal") != std::string::npos && text.find("directx") != std::string::npos;
    convention.baseColorAlphaIsOpacity = mentionsBaseAlphaOpacity;
    convention.specularIsPackedOcclusionRoughnessMetalness = mentionsSpecularPackedOrm;
    convention.normalMapIsDirectX = mentionsDirectXNormals;
    if (mentionsBaseAlphaOpacity || mentionsSpecularPackedOrm || mentionsDirectXNormals) {
        convention.source = readmePath.generic_string();
    }
    return convention;
}

bool fbxMaterialNameImpliesDoubleSided(std::string_view materialName) {
    return containsAnyLower(materialName, {
        "double_sided", "doublesided", "two_sided", "twosided",
        "foliage", "leaf", "leaves", "ivy", "flower", "flowers", "hedge", "hedges",
        "branch", "branches", "plant", "plants", "grass", "vine", "vines", "curtain"
    });
}

bool fbxMaterialNameImpliesAlphaBlend(std::string_view materialName) {
    return containsAnyLower(materialName, {
        "glass", "transparent", "window", "wine", "liquid", "water"
    });
}

struct FbxPysceneMaterialOverride {
    std::optional<float> roughness;
    std::optional<float> metallic;
    std::optional<float> indexOfRefraction;
    std::optional<float> specularTransmission;
    std::optional<bool> doubleSided;
    std::optional<int32_t> nestedPriority;
    std::optional<glm::vec3> volumeAbsorption;
    std::optional<glm::vec3> emissiveFactor;
    float emissiveMultiplier = 1.0f;
    nlohmann::json applied = nlohmann::json::array();
};

struct FbxPysceneOverrides {
    bool found = false;
    std::filesystem::path path;
    std::optional<std::filesystem::path> environmentMap;
    std::optional<float> environmentIntensity;
    float globalEmissiveMultiplier = 1.0f;
    std::unordered_map<std::string, FbxPysceneMaterialOverride> materials;
    nlohmann::json applied = nlohmann::json::array();
    nlohmann::json ignored = nlohmann::json::array();
    std::vector<std::string> warnings;
};

std::optional<std::filesystem::path> siblingFbxPyscenePath(const std::filesystem::path& sourcePath) {
    std::filesystem::path candidate = sourcePath;
    candidate.replace_extension(".pyscene");
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
        return candidate;
    }
    return std::nullopt;
}

std::string stripPythonComment(std::string_view line) {
    bool inSingle = false;
    bool inDouble = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (c == '"' && !inSingle) {
            inDouble = !inDouble;
        } else if (c == '#' && !inSingle && !inDouble) {
            return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}

std::optional<std::string> quotedStringAfter(std::string_view text, std::string_view marker) {
    const size_t markerPos = text.find(marker);
    if (markerPos == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t begin = text.find_first_of("\"'", markerPos + marker.size());
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const char quote = text[begin];
    const size_t end = text.find(quote, begin + 1u);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(text.substr(begin + 1u, end - begin - 1u));
}

std::optional<float> parseFloatValue(std::string_view text) {
    const std::string value = trimAscii(text);
    if (value.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    return end != value.c_str() ? std::optional<float>(parsed) : std::nullopt;
}

std::optional<float> parseFloatAfterOperator(std::string_view line, std::string_view op) {
    const size_t pos = line.find(op);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    return parseFloatValue(line.substr(pos + op.size()));
}

std::optional<bool> parseBoolAfterEquals(std::string_view line) {
    const size_t pos = line.find('=');
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string value = lowerString(trimAscii(line.substr(pos + 1u)));
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    return std::nullopt;
}

std::optional<glm::vec3> parseFloat3(std::string_view line) {
    const size_t begin = line.find("float3");
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t open = line.find('(', begin);
    const size_t close = line.find(')', open == std::string_view::npos ? begin : open);
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
        return std::nullopt;
    }
    std::array<float, 3> values{};
    std::string args(line.substr(open + 1u, close - open - 1u));
    std::replace(args.begin(), args.end(), ',', ' ');
    std::istringstream stream(args);
    if (!(stream >> values[0] >> values[1] >> values[2])) {
        return std::nullopt;
    }
    return glm::vec3{values[0], values[1], values[2]};
}

void setPysceneOverrideValue(
    FbxPysceneMaterialOverride& material,
    std::string_view property,
    std::string_view line,
    nlohmann::json& ignored,
    size_t lineNumber) {
    const std::string prop = std::string(property);
    auto floatValue = [&]() { return parseFloatAfterOperator(line, "="); };
    if (prop == "roughness") {
        material.roughness = floatValue();
    } else if (prop == "metallic" || prop == "metalness") {
        material.metallic = floatValue();
    } else if (prop == "indexOfRefraction") {
        material.indexOfRefraction = floatValue();
    } else if (prop == "specularTransmission") {
        material.specularTransmission = floatValue();
    } else if (prop == "doubleSided") {
        material.doubleSided = parseBoolAfterEquals(line);
    } else if (prop == "nestedPriority") {
        if (const std::optional<float> value = floatValue()) {
            material.nestedPriority = static_cast<int32_t>(*value);
        }
    } else if (prop == "volumeAbsorption") {
        material.volumeAbsorption = parseFloat3(line);
    } else if (prop == "emissiveFactor") {
        if (line.find("*=") != std::string_view::npos) {
            if (const std::optional<float> value = parseFloatAfterOperator(line, "*=")) {
                material.emissiveMultiplier *= *value;
            }
        } else {
            material.emissiveFactor = parseFloat3(line);
        }
    } else {
        ignored.push_back({{"line", lineNumber}, {"statement", std::string(line)}, {"reason", "unsupported-material-property"}});
    }
}

FbxPysceneOverrides parseFbxPysceneOverrides(const std::filesystem::path& sourcePath) {
    FbxPysceneOverrides out;
    const std::optional<std::filesystem::path> sidecar = siblingFbxPyscenePath(sourcePath);
    if (!sidecar.has_value()) {
        return out;
    }
    out.found = true;
    out.path = *sidecar;
    std::ifstream file(*sidecar);
    if (!file.is_open()) {
        out.warnings.push_back("FBX .pyscene sidecar was found but could not be opened: " + sidecar->string());
        return out;
    }

    std::string currentMaterial;
    bool inAllMaterialLoop = false;
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string trimmed = trimAscii(stripPythonComment(line));
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.find("sceneBuilder.importScene") != std::string::npos) {
            continue;
        }
        if (trimmed.find("for m in sceneBuilder.materials") != std::string::npos) {
            inAllMaterialLoop = true;
            continue;
        }
        if (!line.empty() && !std::isspace(static_cast<unsigned char>(line.front()))) {
            inAllMaterialLoop = false;
        }
        if (trimmed.find("sceneBuilder.envMap") != std::string::npos && trimmed.find("EnvMap") != std::string::npos) {
            if (const std::optional<std::string> value = quotedStringAfter(trimmed, "EnvMap")) {
                out.environmentMap = std::filesystem::path(*value);
                out.applied.push_back({{"line", lineNumber}, {"property", "envMap"}, {"value", *value}});
            }
            continue;
        }
        if (trimmed.find("sceneBuilder.envMap.intensity") != std::string::npos) {
            out.environmentIntensity = parseFloatAfterOperator(trimmed, "=");
            out.applied.push_back({{"line", lineNumber}, {"property", "envMap.intensity"}, {"value", out.environmentIntensity.value_or(0.0f)}});
            continue;
        }
        if (const std::optional<std::string> materialName = quotedStringAfter(trimmed, "getMaterial")) {
            currentMaterial = *materialName;
            if (const size_t close = trimmed.find(')'); close != std::string::npos) {
                const size_t dot = trimmed.find('.', close);
                if (dot != std::string::npos) {
                    const size_t op = trimmed.find_first_of("= ", dot + 1u);
                    const std::string property = op == std::string::npos
                        ? trimmed.substr(dot + 1u)
                        : trimmed.substr(dot + 1u, op - dot - 1u);
                    setPysceneOverrideValue(out.materials[currentMaterial], property, trimmed, out.ignored, lineNumber);
                }
            }
            continue;
        }
        if (trimmed.rfind("m.", 0) == 0) {
            const size_t op = trimmed.find_first_of("= ");
            const std::string property = op == std::string::npos ? trimmed.substr(2u) : trimmed.substr(2u, op - 2u);
            if (inAllMaterialLoop && property == "emissiveFactor" && trimmed.find("*=") != std::string::npos) {
                if (const std::optional<float> value = parseFloatAfterOperator(trimmed, "*=")) {
                    out.globalEmissiveMultiplier *= *value;
                    out.applied.push_back({{"line", lineNumber}, {"property", "global.emissiveFactorMultiplier"}, {"value", *value}});
                }
            } else if (!currentMaterial.empty()) {
                setPysceneOverrideValue(out.materials[currentMaterial], property, trimmed, out.ignored, lineNumber);
            } else {
                out.ignored.push_back({{"line", lineNumber}, {"statement", trimmed}, {"reason", "material-target-not-resolved"}});
            }
            continue;
        }
        out.ignored.push_back({{"line", lineNumber}, {"statement", trimmed}, {"reason", "unsupported-statement"}});
    }
    return out;
}

glm::vec3 attenuationColorFromAbsorption(glm::vec3 absorption) {
    return glm::clamp(glm::exp(-glm::max(absorption, glm::vec3{0.0f})), glm::vec3{0.0f}, glm::vec3{1.0f});
}

void applyFbxPysceneMaterialOverride(MaterialAsset& material, const FbxPysceneMaterialOverride& overrideData) {
    if (overrideData.roughness.has_value()) {
        material.roughnessFactor = std::clamp(*overrideData.roughness, 0.0f, 1.0f);
    }
    if (overrideData.metallic.has_value()) {
        material.metallicFactor = std::clamp(*overrideData.metallic, 0.0f, 1.0f);
    }
    if (overrideData.indexOfRefraction.has_value()) {
        material.hasIor = 1u;
        material.iorFactor = std::max(*overrideData.indexOfRefraction, 1.0f);
    }
    if (overrideData.specularTransmission.has_value()) {
        material.hasTransmission = 1u;
        material.transmissionFactor = std::clamp(*overrideData.specularTransmission, 0.0f, 1.0f);
        if (material.transmissionFactor > 0.0f) {
            material.alphaMode = kMaterialAlphaModeBlend;
            material.materialWorkflow = kMaterialWorkflowMetallicRoughness;
        }
    }
    if (overrideData.doubleSided.has_value()) {
        material.doubleSided = *overrideData.doubleSided ? 1u : 0u;
    }
    if (overrideData.nestedPriority.has_value()) {
        material.nestedPriority = *overrideData.nestedPriority;
    }
    if (overrideData.volumeAbsorption.has_value()) {
        material.hasVolume = 1u;
        material.volumeAttenuationColor = attenuationColorFromAbsorption(*overrideData.volumeAbsorption);
        material.volumeAttenuationDistance = 1.0f;
    }
    if (overrideData.emissiveFactor.has_value()) {
        material.emissiveFactor = *overrideData.emissiveFactor;
        material.hasEmissiveStrength = 1u;
    }
    if (overrideData.emissiveMultiplier != 1.0f) {
        material.emissiveFactor *= overrideData.emissiveMultiplier;
        material.hasEmissiveStrength = 1u;
    }
}

bool textureFormatHasAlphaChannel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

bool textureFormatIsBc3(VkFormat format) {
    return format == VK_FORMAT_BC3_UNORM_BLOCK || format == VK_FORMAT_BC3_SRGB_BLOCK;
}

bool textureFormatIsBc1Rgba(VkFormat format) {
    return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
}

bool bc3BlockHasNonOpaqueAlpha(const uint8_t* block) {
    uint8_t palette[8]{};
    palette[0] = block[0];
    palette[1] = block[1];
    if (palette[0] > palette[1]) {
        for (uint32_t i = 1; i <= 6; ++i) {
            palette[i + 1u] = static_cast<uint8_t>(((7u - i) * palette[0] + i * palette[1] + 3u) / 7u);
        }
    } else {
        for (uint32_t i = 1; i <= 4; ++i) {
            palette[i + 1u] = static_cast<uint8_t>(((5u - i) * palette[0] + i * palette[1] + 2u) / 5u);
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t indices = 0;
    for (uint32_t i = 0; i < 6u; ++i) {
        indices |= static_cast<uint64_t>(block[2u + i]) << (8u * i);
    }
    for (uint32_t texel = 0; texel < 16u; ++texel) {
        const uint32_t index = static_cast<uint32_t>((indices >> (3u * texel)) & 0x7ull);
        if (palette[index] < 250u) {
            return true;
        }
    }
    return false;
}

bool bc1BlockHasTransparentTexel(const uint8_t* block) {
    const uint16_t color0 = static_cast<uint16_t>(block[0]) | (static_cast<uint16_t>(block[1]) << 8u);
    const uint16_t color1 = static_cast<uint16_t>(block[2]) | (static_cast<uint16_t>(block[3]) << 8u);
    if (color0 > color1) {
        return false;
    }
    const uint32_t indices = static_cast<uint32_t>(block[4]) |
        (static_cast<uint32_t>(block[5]) << 8u) |
        (static_cast<uint32_t>(block[6]) << 16u) |
        (static_cast<uint32_t>(block[7]) << 24u);
    for (uint32_t texel = 0; texel < 16u; ++texel) {
        if (((indices >> (2u * texel)) & 0x3u) == 3u) {
            return true;
        }
    }
    return false;
}

bool compressedTextureMipHasNonOpaqueAlpha(const TextureAsset& texture, uint32_t blockBytes, bool bc3) {
    if (texture.rgba8.empty() || texture.mipData.empty()) {
        return false;
    }
    const TextureMipLevel& mip = texture.mipData.front();
    if (mip.offset > texture.rgba8.size() || mip.size > texture.rgba8.size() - mip.offset) {
        return false;
    }
    const uint8_t* bytes = texture.rgba8.data() + mip.offset;
    const uint64_t blockCount = mip.size / blockBytes;
    for (uint64_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        const uint8_t* block = bytes + blockIndex * blockBytes;
        if (bc3 ? bc3BlockHasNonOpaqueAlpha(block) : bc1BlockHasTransparentTexel(block)) {
            return true;
        }
    }
    return false;
}

bool textureAssetHasNonOpaqueAlpha(const TextureAsset& texture) {
    const VkFormat format = texture.compressedFormat != VK_FORMAT_UNDEFINED ? texture.compressedFormat : texture.format;
    if (!textureFormatHasAlphaChannel(format)) {
        return false;
    }
    if (textureFormatIsBc3(format)) {
        return compressedTextureMipHasNonOpaqueAlpha(texture, 16u, true);
    }
    if (textureFormatIsBc1Rgba(format)) {
        return compressedTextureMipHasNonOpaqueAlpha(texture, 8u, false);
    }
    if (!texture.isCompressed && !texture.rgba8.empty()) {
        const size_t texelCount = texture.rgba8.size() / 4u;
        for (size_t texel = 0; texel < texelCount; ++texel) {
            if (texture.rgba8[texel * 4u + 3u] < 250u) {
                return true;
            }
        }
    }
    return false;
}

bool textureAssetSourceLooksDds(const TextureAsset& texture) {
    return lowerString(texture.sourcePath.extension().string()) == ".dds" || lowerString(texture.sourceContainerKind) == "dds";
}

const char* assimpTextureMapModeName(aiTextureMapMode mode) {
    switch (mode) {
    case aiTextureMapMode_Wrap: return "Wrap";
    case aiTextureMapMode_Clamp: return "Clamp";
    case aiTextureMapMode_Decal: return "Decal";
    case aiTextureMapMode_Mirror: return "Mirror";
    default: return "Unknown";
    }
}

TextureWrap textureWrapFromAssimp(aiTextureMapMode mode) {
    switch (mode) {
    case aiTextureMapMode_Clamp:
    case aiTextureMapMode_Decal:
        return TextureWrap::ClampToEdge;
    case aiTextureMapMode_Mirror:
        return TextureWrap::MirroredRepeat;
    case aiTextureMapMode_Wrap:
    default:
        return TextureWrap::Repeat;
    }
}

void applyFbxTextureSampler(TextureAsset& texture, const aiTextureMapMode* mapModes) {
    if (mapModes == nullptr) {
        return;
    }
    texture.sampler.wrapS = textureWrapFromAssimp(mapModes[0]);
    texture.sampler.wrapT = textureWrapFromAssimp(mapModes[1]);
}

TextureTransformAsset fbxTextureTransformFromAssimp(
    const aiMaterial* materialSource,
    aiTextureType type,
    unsigned int index,
    unsigned int uvIndex,
    const std::string& materialName,
    const char* slotName,
    std::vector<std::string>& warnings) {
    TextureTransformAsset transform;
    transform.texCoord = std::min<uint32_t>(uvIndex, 1u);
    if (uvIndex > 1u) {
        warnings.push_back("FBX material '" + materialName + "' " + slotName + " texture uses UV channel " + std::to_string(uvIndex) + "; engine runtime supports UV0/UV1, clamping to UV1.");
    }

    aiUVTransform uvTransform;
    if (materialSource != nullptr && materialSource->Get(AI_MATKEY_UVTRANSFORM(type, index), uvTransform) == AI_SUCCESS) {
        const glm::vec2 translation{uvTransform.mTranslation.x, uvTransform.mTranslation.y};
        const glm::vec2 scale{uvTransform.mScaling.x, uvTransform.mScaling.y};
        const float rotation = uvTransform.mRotation;
        transform.scale = scale;
        transform.rotation = rotation;
        if (std::abs(rotation) > 1.0e-7f) {
            const float c = std::cos(rotation);
            const float s = std::sin(rotation);
            const glm::vec2 center{0.5f, 0.5f};
            const glm::vec2 rotatedCenter{
                c * center.x - s * center.y,
                s * center.x + c * center.y,
            };
            transform.offset = translation + center - rotatedCenter;
        } else {
            transform.offset = translation;
        }
    }
    transform.enabled =
        std::abs(transform.offset.x) > 1.0e-7f ||
        std::abs(transform.offset.y) > 1.0e-7f ||
        std::abs(transform.scale.x - 1.0f) > 1.0e-7f ||
        std::abs(transform.scale.y - 1.0f) > 1.0e-7f ||
        std::abs(transform.rotation) > 1.0e-7f ||
        transform.texCoord != 0u;
    return transform;
}

std::optional<std::filesystem::path> resolveFbxTexturePath(
    const std::filesystem::path& sourcePath,
    const std::string& token,
    std::string& reason) {
    if (token.empty()) {
        reason = "empty texture path";
        return std::nullopt;
    }
    if (token[0] == '*') {
        reason = "embedded FBX texture payloads are not decoded by this slice";
        return std::nullopt;
    }
    if (token.find("://") != std::string::npos || isDataUri(token)) {
        reason = "URI texture references are unsupported for FBX native cook";
        return std::nullopt;
    }

    std::filesystem::path texturePath = std::filesystem::path(token).lexically_normal();
    std::error_code ec;
    if (texturePath.is_absolute()) {
        if (std::filesystem::exists(texturePath, ec)) {
            return texturePath;
        }
    } else {
        const std::filesystem::path relativeCandidate = (sourcePath.parent_path() / texturePath).lexically_normal();
        if (std::filesystem::exists(relativeCandidate, ec)) {
            return relativeCandidate;
        }
    }

    const std::filesystem::path filenameCandidate = sourcePath.parent_path() / texturePath.filename();
    if (!texturePath.filename().empty() && std::filesystem::exists(filenameCandidate, ec)) {
        return filenameCandidate.lexically_normal();
    }

    const std::filesystem::path texturesDirectoryCandidate = sourcePath.parent_path() / "Textures" / texturePath.filename();
    if (!texturePath.filename().empty() && std::filesystem::exists(texturesDirectoryCandidate, ec)) {
        return texturesDirectoryCandidate.lexically_normal();
    }

    reason = "texture file not found";
    return std::nullopt;
}

std::string embeddedFbxTextureName(const std::filesystem::path& sourcePath, const std::string& token, NativeTextureRole role) {
    std::string name = sourcePath.stem().string();
    if (name.empty()) {
        name = "fbx";
    }
    name += "_embedded_";
    name += token.empty() ? std::string("texture") : token;
    name += "_";
    name += nativeTextureRoleName(role);
    return safeStem(name);
}

std::string embeddedFbxTextureFormatHint(const aiTexture* texture) {
    if (texture == nullptr || texture->achFormatHint[0] == '\0') {
        return {};
    }
    size_t length = 0;
    while (length < sizeof(texture->achFormatHint) && texture->achFormatHint[length] != '\0') {
        ++length;
    }
    return std::string(texture->achFormatHint, length);
}

int64_t quantizeFbxNormalComponent(float value) {
    return static_cast<int64_t>(std::llround(static_cast<double>(value) * 1000000.0));
}

std::string quantizedFbxVec3Key(const glm::vec3& value) {
    return std::to_string(quantizeFbxNormalComponent(value.x)) + ":" +
        std::to_string(quantizeFbxNormalComponent(value.y)) + ":" +
        std::to_string(quantizeFbxNormalComponent(value.z));
}

nlohmann::json fbxTransformDiagnostics(const aiNode* node, const glm::mat4& transform) {
    const glm::vec3 translation{transform[3].x, transform[3].y, transform[3].z};
    const glm::vec3 basisX{transform[0].x, transform[0].y, transform[0].z};
    const glm::vec3 basisY{transform[1].x, transform[1].y, transform[1].z};
    const glm::vec3 basisZ{transform[2].x, transform[2].y, transform[2].z};
    const glm::vec3 scale{
        glm::length(basisX),
        glm::length(basisY),
        glm::length(basisZ),
    };
    const float determinant = glm::determinant(glm::mat3(transform));
    const bool negativeScale = determinant < 0.0f;
    const bool nonUniformScale =
        std::abs(scale.x - scale.y) > 1.0e-5f ||
        std::abs(scale.y - scale.z) > 1.0e-5f;

    nlohmann::json metadataKeys = nlohmann::json::array();
    nlohmann::json authoredFbxOps = nlohmann::json::array();
    auto addAuthoredOp = [&](const std::string& op) {
        for (const nlohmann::json& existing : authoredFbxOps) {
            if (existing.is_string() && existing.get<std::string>() == op) {
                return;
            }
        }
        authoredFbxOps.push_back(op);
    };
    if (node != nullptr && node->mMetaData != nullptr) {
        const aiMetadata& metadata = *node->mMetaData;
        for (unsigned i = 0; i < metadata.mNumProperties; ++i) {
            const std::string key = assimpString(metadata.mKeys[i]);
            const std::string normalized = normalizeMetadataKey(key);
            metadataKeys.push_back({{"key", key}, {"normalizedKey", normalized}});
            if (normalized.find("rotationpivot") != std::string::npos) addAuthoredOp("rotationPivot");
            if (normalized.find("scalingpivot") != std::string::npos) addAuthoredOp("scalingPivot");
            if (normalized.find("rotationoffset") != std::string::npos) addAuthoredOp("rotationOffset");
            if (normalized.find("scalingoffset") != std::string::npos) addAuthoredOp("scalingOffset");
            if (normalized.find("prerotation") != std::string::npos) addAuthoredOp("preRotation");
            if (normalized.find("postrotation") != std::string::npos) addAuthoredOp("postRotation");
            if (normalized.find("geometrictranslation") != std::string::npos) addAuthoredOp("geometricTranslation");
            if (normalized.find("geometricrotation") != std::string::npos) addAuthoredOp("geometricRotation");
            if (normalized.find("geometricscaling") != std::string::npos) addAuthoredOp("geometricScaling");
        }
    }

    return {
        {"policy", "assimp_fbx_node_transform_with_pivot_prepost_geometric_ops_baked"},
        {"matrixPreserved", true},
        {"decompositionReported", true},
        {"negativeScaleHandled", true},
        {"translation", vec3ArrayJson(translation)},
        {"scaleMagnitude", vec3ArrayJson(scale)},
        {"nonUniformScale", nonUniformScale},
        {"determinant", determinant},
        {"negativeScale", negativeScale},
        {"handedness", negativeScale ? "mirrored" : "regular"},
        {"authoredFbxOps", authoredFbxOps},
        {"authoredFbxOpCount", authoredFbxOps.size()},
        {"metadataKeys", metadataKeys},
        {"pivotPrePostRotationPolicy", "Assimp composes FBX pivots, offsets, pre/post rotations, and geometric transforms into aiNode::mTransformation; raw metadata keys are retained when exposed by Assimp."},
    };
}

nlohmann::json fbxHardEdgeDiagnostics(const MeshAsset& mesh, bool sourceHasNormals) {
    std::unordered_map<std::string, std::unordered_set<std::string>> normalsByPosition;
    normalsByPosition.reserve(mesh.vertices.size());
    for (const MeshVertex& vertex : mesh.vertices) {
        normalsByPosition[quantizedFbxVec3Key(vertex.position)].insert(quantizedFbxVec3Key(vertex.normal));
    }
    size_t sharedPositionCount = 0;
    size_t hardEdgePositionCount = 0;
    size_t maxNormalCountAtPosition = 0;
    for (const auto& entry : normalsByPosition) {
        const size_t normalCount = entry.second.size();
        if (normalCount > 1u) {
            ++sharedPositionCount;
            ++hardEdgePositionCount;
        }
        maxNormalCountAtPosition = std::max(maxNormalCountAtPosition, normalCount);
    }
    return {
        {"sourceNormalsPresent", sourceHasNormals},
        {"policy", sourceHasNormals ? "preserve_assimp_split_normals" : "assimp_generated_smooth_normals"},
        {"hardEdgePreservationImplemented", true},
        {"hardEdgeDetection", "same_position_with_multiple_normals"},
        {"uniquePositionCount", normalsByPosition.size()},
        {"sharedPositionWithMultipleNormalsCount", sharedPositionCount},
        {"hardEdgePositionCount", hardEdgePositionCount},
        {"maxNormalCountAtPosition", maxNormalCountAtPosition},
        {"smoothingGroupIdsExposedByAssimp", false},
        {"smoothingGroupPolicy", "FBX smoothing group IDs are not exposed by Assimp in aiMesh; authored hard edges are preserved when they arrive as split normals and reported by position/normal discontinuity."},
    };
}

TextureData loadEmbeddedFbxTextureData(
    const aiTexture* texture,
    const NativeTextureFormatSupport& textureFormatSupport,
    NativeTextureRole role,
    NativeTextureColorSpace colorSpace,
    std::string& storage) {
    if (texture == nullptr || texture->pcData == nullptr || texture->mWidth == 0) {
        throw std::runtime_error("FBX embedded texture is empty");
    }
    if (texture->mHeight == 0) {
        storage = "compressed-bytes";
        const auto* bytes = reinterpret_cast<const uint8_t*>(texture->pcData);
        return TextureLoader::load(bytes, static_cast<size_t>(texture->mWidth), textureFormatSupport, role, colorSpace);
    }

    storage = "raw-aiTexel-bgra";
    TextureData result;
    result.width = static_cast<int>(texture->mWidth);
    result.height = static_cast<int>(texture->mHeight);
    result.mipLevels = 1;
    result.format = VK_FORMAT_R8G8B8A8_UNORM;
    result.linearColorSpace = colorSpace != NativeTextureColorSpace::Srgb;
    const size_t texelCount = static_cast<size_t>(texture->mWidth) * static_cast<size_t>(texture->mHeight);
    result.pixels.resize(texelCount * 4u);
    for (size_t i = 0; i < texelCount; ++i) {
        const aiTexel& texel = texture->pcData[i];
        result.pixels[i * 4u + 0u] = texel.r;
        result.pixels[i * 4u + 1u] = texel.g;
        result.pixels[i * 4u + 2u] = texel.b;
        result.pixels[i * 4u + 3u] = texel.a;
    }
    return result;
}

nlohmann::json assimpVector3Json(const aiVector3D& value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

nlohmann::json assimpQuaternionJson(const aiQuaternion& value) {
    return nlohmann::json::array({value.x, value.y, value.z, value.w});
}

nlohmann::json assimpPositionKeysJson(const aiVectorKey* keys, unsigned count) {
    nlohmann::json out = nlohmann::json::array();
    for (unsigned i = 0; i < count; ++i) {
        out.push_back({
            {"time", keys[i].mTime},
            {"value", assimpVector3Json(keys[i].mValue)},
        });
    }
    return out;
}

nlohmann::json assimpRotationKeysJson(const aiQuatKey* keys, unsigned count) {
    nlohmann::json out = nlohmann::json::array();
    for (unsigned i = 0; i < count; ++i) {
        out.push_back({
            {"time", keys[i].mTime},
            {"value", assimpQuaternionJson(keys[i].mValue)},
        });
    }
    return out;
}

void collectAssimpNodeIndices(const aiNode* node, std::unordered_map<std::string, uint32_t>& indices) {
    if (node == nullptr) {
        return;
    }
    const uint32_t index = static_cast<uint32_t>(indices.size());
    const std::string name = assimpString(node->mName);
    if (!name.empty()) {
        indices.emplace(name, index);
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        collectAssimpNodeIndices(node->mChildren[i], indices);
    }
}

void collectAssimpMeshNodeIndices(
    const aiNode* node,
    std::unordered_map<unsigned, uint32_t>& nodeIndexByMesh,
    std::unordered_map<std::string, uint32_t>& nodeIndexByMeshName,
    const aiScene* scene,
    uint32_t& nextNodeIndex) {
    if (node == nullptr) {
        return;
    }
    const uint32_t nodeIndex = nextNodeIndex++;
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        const unsigned meshIndex = node->mMeshes[i];
        nodeIndexByMesh.try_emplace(meshIndex, nodeIndex);
        if (scene != nullptr && meshIndex < scene->mNumMeshes && scene->mMeshes[meshIndex] != nullptr) {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (mesh->mName.length > 0) {
                nodeIndexByMeshName.try_emplace(assimpString(mesh->mName), nodeIndex);
            }
        }
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        collectAssimpMeshNodeIndices(node->mChildren[i], nodeIndexByMesh, nodeIndexByMeshName, scene, nextNodeIndex);
    }
}

nlohmann::json assimpVectorKeyDecodedTrackJson(
    const aiVectorKey* keys,
    unsigned count,
    double ticksPerSecond,
    const char* targetPath,
    const char* interpolation) {
    nlohmann::json times = nlohmann::json::array();
    nlohmann::json values = nlohmann::json::array();
    for (unsigned i = 0; i < count; ++i) {
        times.push_back(ticksPerSecond > 0.0 ? keys[i].mTime / ticksPerSecond : keys[i].mTime);
        values.push_back(assimpVector3Json(keys[i].mValue));
    }
    return {
        {"decoded", count > 0},
        {"targetPath", targetPath},
        {"interpolation", interpolation},
        {"times", times},
        {"values", values},
    };
}

nlohmann::json assimpQuatKeyDecodedTrackJson(
    const aiQuatKey* keys,
    unsigned count,
    double ticksPerSecond,
    const char* interpolation) {
    nlohmann::json times = nlohmann::json::array();
    nlohmann::json values = nlohmann::json::array();
    for (unsigned i = 0; i < count; ++i) {
        times.push_back(ticksPerSecond > 0.0 ? keys[i].mTime / ticksPerSecond : keys[i].mTime);
        values.push_back(assimpQuaternionJson(keys[i].mValue));
    }
    return {
        {"decoded", count > 0},
        {"targetPath", "rotation"},
        {"interpolation", interpolation},
        {"times", times},
        {"values", values},
    };
}

nlohmann::json assimpMorphKeysDecodedTrackJson(
    const aiMeshMorphKey* keys,
    unsigned count,
    double ticksPerSecond,
    size_t morphTargetCount) {
    nlohmann::json times = nlohmann::json::array();
    nlohmann::json values = nlohmann::json::array();
    size_t maxWeightIndex = morphTargetCount;
    for (unsigned keyIndex = 0; keyIndex < count; ++keyIndex) {
        const aiMeshMorphKey& key = keys[keyIndex];
        for (unsigned valueIndex = 0; valueIndex < key.mNumValuesAndWeights; ++valueIndex) {
            maxWeightIndex = std::max(maxWeightIndex, static_cast<size_t>(key.mValues[valueIndex]) + 1u);
        }
    }
    for (unsigned keyIndex = 0; keyIndex < count; ++keyIndex) {
        const aiMeshMorphKey& key = keys[keyIndex];
        times.push_back(ticksPerSecond > 0.0 ? key.mTime / ticksPerSecond : key.mTime);
        std::vector<float> weights(maxWeightIndex, 0.0f);
        for (unsigned valueIndex = 0; valueIndex < key.mNumValuesAndWeights; ++valueIndex) {
            const unsigned targetIndex = key.mValues[valueIndex];
            if (targetIndex < weights.size()) {
                weights[targetIndex] = static_cast<float>(key.mWeights[valueIndex]);
            }
        }
        nlohmann::json weightJson = nlohmann::json::array();
        for (float weight : weights) {
            weightJson.push_back(weight);
        }
        values.push_back(std::move(weightJson));
    }
    return {
        {"decoded", count > 0 && maxWeightIndex > 0u},
        {"targetPath", "weights"},
        {"interpolation", "LINEAR"},
        {"times", times},
        {"values", values},
        {"weightCount", maxWeightIndex},
    };
}

void appendFbxDecodedTrackChannel(
    nlohmann::json& channels,
    uint32_t& decodedChannelCount,
    uint32_t& decodedKeyframeCount,
    const std::string& targetNodeName,
    int targetNodeIndex,
    const char* targetPath,
    nlohmann::json decodedTrack,
    uint32_t keyframeCount) {
    if (keyframeCount == 0) {
        return;
    }
    decodedChannelCount += 1u;
    decodedKeyframeCount += keyframeCount;
    channels.push_back({
        {"index", channels.size()},
        {"targetNode", targetNodeName},
        {"target", {
            {"node", targetNodeIndex},
            {"nodeName", targetNodeName},
            {"path", targetPath},
        }},
        {"decodedTrack", std::move(decodedTrack)},
    });
}

void collectAssimpNodeParents(const aiNode* node, const std::string& parentName, std::unordered_map<std::string, std::string>& parents) {
    if (node == nullptr) {
        return;
    }
    const std::string nodeName = assimpString(node->mName);
    if (!nodeName.empty()) {
        parents[nodeName] = parentName;
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        collectAssimpNodeParents(node->mChildren[i], nodeName, parents);
    }
}

nlohmann::json fbxSkeletonMetadataFromAssimp(const aiScene* imported) {
    nlohmann::json skeletons = nlohmann::json::array();
    if (imported == nullptr || !imported->HasMeshes()) {
        return skeletons;
    }

    std::unordered_map<std::string, std::string> parentByNodeName;
    collectAssimpNodeParents(imported->mRootNode, {}, parentByNodeName);

    struct BoneSummary {
        std::string name;
        std::string parentName;
        glm::mat4 offsetMatrix{1.0f};
        uint32_t weightCount = 0;
        nlohmann::json meshIndices = nlohmann::json::array();
        nlohmann::json weightSamples = nlohmann::json::array();
    };
    std::vector<BoneSummary> bones;
    std::unordered_map<std::string, size_t> boneIndexByName;
    nlohmann::json skinnedMeshes = nlohmann::json::array();

    for (unsigned meshIndex = 0; meshIndex < imported->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = imported->mMeshes[meshIndex];
        if (mesh == nullptr || !mesh->HasBones()) {
            continue;
        }
        nlohmann::json meshBoneNames = nlohmann::json::array();
        for (unsigned boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex) {
            const aiBone* bone = mesh->mBones[boneIndex];
            if (bone == nullptr) {
                continue;
            }
            const std::string boneName = assimpString(bone->mName);
            if (boneName.empty()) {
                continue;
            }
            meshBoneNames.push_back(boneName);
            size_t summaryIndex = 0;
            const auto existing = boneIndexByName.find(boneName);
            if (existing == boneIndexByName.end()) {
                summaryIndex = bones.size();
                boneIndexByName.emplace(boneName, summaryIndex);
                BoneSummary summary;
                summary.name = boneName;
                const auto parentIt = parentByNodeName.find(boneName);
                summary.parentName = parentIt != parentByNodeName.end() ? parentIt->second : std::string{};
                summary.offsetMatrix = assimpMatrixToGlm(bone->mOffsetMatrix);
                bones.push_back(std::move(summary));
            } else {
                summaryIndex = existing->second;
            }

            BoneSummary& summary = bones[summaryIndex];
            bool meshAlreadyRecorded = false;
            for (const nlohmann::json& value : summary.meshIndices) {
                if (value.is_number_unsigned() && value.get<unsigned>() == meshIndex) {
                    meshAlreadyRecorded = true;
                    break;
                }
            }
            if (!meshAlreadyRecorded) {
                summary.meshIndices.push_back(meshIndex);
            }
            summary.weightCount += bone->mNumWeights;
            for (unsigned weightIndex = 0; weightIndex < bone->mNumWeights && summary.weightSamples.size() < 8; ++weightIndex) {
                const aiVertexWeight& weight = bone->mWeights[weightIndex];
                summary.weightSamples.push_back({
                    {"meshIndex", meshIndex},
                    {"vertexId", weight.mVertexId},
                    {"weight", weight.mWeight},
                });
            }
        }
        skinnedMeshes.push_back({
            {"meshIndex", meshIndex},
            {"meshName", mesh->mName.length > 0 ? assimpString(mesh->mName) : ("FbxMesh_" + std::to_string(meshIndex))},
            {"boneCount", mesh->mNumBones},
            {"bones", meshBoneNames},
        });
    }

    if (bones.empty()) {
        return skeletons;
    }
    nlohmann::json joints = nlohmann::json::array();
    for (size_t i = 0; i < bones.size(); ++i) {
        const BoneSummary& bone = bones[i];
        const auto parentIt = boneIndexByName.find(bone.parentName);
        joints.push_back({
            {"index", i},
            {"name", bone.name},
            {"parentName", bone.parentName},
            {"parentIndex", parentIt != boneIndexByName.end() ? static_cast<int>(parentIt->second) : -1},
            {"inverseBindMatrix", matrixJson(bone.offsetMatrix)},
            {"meshIndices", bone.meshIndices},
            {"weightCount", bone.weightCount},
            {"weightSamples", bone.weightSamples},
        });
    }
    skeletons.push_back({
        {"schema", "FbxSkeletonMetadataV1"},
        {"index", 0},
        {"name", "FbxSkeleton_0"},
        {"jointCount", joints.size()},
        {"joints", joints},
        {"skinnedMeshCount", skinnedMeshes.size()},
        {"skinnedMeshes", skinnedMeshes},
        {"runtimeSupport", "metadata_bridge_runtime_skinning_supported"},
    });
    return skeletons;
}

nlohmann::json fbxAnimationMetadataFromAssimp(const aiScene* imported) {
    nlohmann::json animations = nlohmann::json::array();
    if (imported == nullptr || !imported->HasAnimations()) {
        return animations;
    }
    std::unordered_map<std::string, uint32_t> nodeIndexByName;
    collectAssimpNodeIndices(imported->mRootNode, nodeIndexByName);
    std::unordered_map<unsigned, uint32_t> nodeIndexByMesh;
    std::unordered_map<std::string, uint32_t> nodeIndexByMeshName;
    uint32_t nextNodeIndex = 0;
    collectAssimpMeshNodeIndices(imported->mRootNode, nodeIndexByMesh, nodeIndexByMeshName, imported, nextNodeIndex);
    for (unsigned animationIndex = 0; animationIndex < imported->mNumAnimations; ++animationIndex) {
        const aiAnimation* animation = imported->mAnimations[animationIndex];
        if (animation == nullptr) {
            continue;
        }
        nlohmann::json channels = nlohmann::json::array();
        uint32_t decodedChannelCount = 0;
        uint32_t decodedMorphChannelCount = 0;
        uint32_t decodedKeyframeCount = 0;
        const double ticksPerSecond = animation->mTicksPerSecond > 0.0 ? animation->mTicksPerSecond : 25.0;
        for (unsigned channelIndex = 0; channelIndex < animation->mNumChannels; ++channelIndex) {
            const aiNodeAnim* channel = animation->mChannels[channelIndex];
            if (channel == nullptr) {
                continue;
            }
            const std::string targetNodeName = assimpString(channel->mNodeName);
            const auto nodeIt = nodeIndexByName.find(targetNodeName);
            const int targetNodeIndex = nodeIt != nodeIndexByName.end() ? static_cast<int>(nodeIt->second) : -1;
            appendFbxDecodedTrackChannel(
                channels,
                decodedChannelCount,
                decodedKeyframeCount,
                targetNodeName,
                targetNodeIndex,
                "translation",
                assimpVectorKeyDecodedTrackJson(channel->mPositionKeys, channel->mNumPositionKeys, ticksPerSecond, "translation", "LINEAR"),
                channel->mNumPositionKeys);
            appendFbxDecodedTrackChannel(
                channels,
                decodedChannelCount,
                decodedKeyframeCount,
                targetNodeName,
                targetNodeIndex,
                "rotation",
                assimpQuatKeyDecodedTrackJson(channel->mRotationKeys, channel->mNumRotationKeys, ticksPerSecond, "LINEAR"),
                channel->mNumRotationKeys);
            appendFbxDecodedTrackChannel(
                channels,
                decodedChannelCount,
                decodedKeyframeCount,
                targetNodeName,
                targetNodeIndex,
                "scale",
                assimpVectorKeyDecodedTrackJson(channel->mScalingKeys, channel->mNumScalingKeys, ticksPerSecond, "scale", "LINEAR"),
                channel->mNumScalingKeys);
        }
        for (unsigned morphChannelIndex = 0; morphChannelIndex < animation->mNumMorphMeshChannels; ++morphChannelIndex) {
            const aiMeshMorphAnim* channel = animation->mMorphMeshChannels[morphChannelIndex];
            if (channel == nullptr || channel->mNumKeys == 0) {
                continue;
            }
            const std::string targetMeshName = assimpString(channel->mName);
            int targetNodeIndex = -1;
            if (const auto meshNodeIt = nodeIndexByMeshName.find(targetMeshName); meshNodeIt != nodeIndexByMeshName.end()) {
                targetNodeIndex = static_cast<int>(meshNodeIt->second);
            } else if (const auto nodeIt = nodeIndexByName.find(targetMeshName); nodeIt != nodeIndexByName.end()) {
                targetNodeIndex = static_cast<int>(nodeIt->second);
            }
            size_t morphTargetCount = 0;
            for (unsigned meshIndex = 0; meshIndex < imported->mNumMeshes; ++meshIndex) {
                const aiMesh* mesh = imported->mMeshes[meshIndex];
                if (mesh != nullptr && mesh->mName.length > 0 && assimpString(mesh->mName) == targetMeshName) {
                    morphTargetCount = std::max(morphTargetCount, static_cast<size_t>(mesh->mNumAnimMeshes));
                    if (targetNodeIndex < 0) {
                        if (const auto meshNodeIt = nodeIndexByMesh.find(meshIndex); meshNodeIt != nodeIndexByMesh.end()) {
                            targetNodeIndex = static_cast<int>(meshNodeIt->second);
                        }
                    }
                }
            }
            nlohmann::json decodedTrack = assimpMorphKeysDecodedTrackJson(channel->mKeys, channel->mNumKeys, ticksPerSecond, morphTargetCount);
            if (!decodedTrack.value("decoded", false)) {
                continue;
            }
            decodedChannelCount += 1u;
            decodedMorphChannelCount += 1u;
            decodedKeyframeCount += channel->mNumKeys;
            channels.push_back({
                {"index", channels.size()},
                {"targetNode", targetMeshName},
                {"targetMesh", targetMeshName},
                {"target", {
                    {"node", targetNodeIndex},
                    {"nodeName", targetMeshName},
                    {"meshName", targetMeshName},
                    {"path", "weights"},
                }},
                {"decoded", true},
                {"keyframeCount", channel->mNumKeys},
                {"decodedTrack", std::move(decodedTrack)},
            });
        }
        animations.push_back({
            {"schema", "FbxAnimationMetadataV1"},
            {"index", animationIndex},
            {"name", animation->mName.length > 0 ? assimpString(animation->mName) : ("FbxAnimation_" + std::to_string(animationIndex))},
            {"durationTicks", animation->mDuration},
            {"ticksPerSecond", ticksPerSecond},
            {"durationSeconds", ticksPerSecond > 0.0 ? animation->mDuration / ticksPerSecond : 0.0},
            {"channelCount", animation->mNumChannels},
            {"meshMorphChannelCount", animation->mNumMorphMeshChannels},
            {"decodedChannelCount", decodedChannelCount},
            {"decodedMorphChannelCount", decodedMorphChannelCount},
            {"decodedKeyframeCount", decodedKeyframeCount},
            {"channels", channels},
            {"clip", {
                {"startTime", 0.0},
                {"endTime", ticksPerSecond > 0.0 ? animation->mDuration / ticksPerSecond : animation->mDuration},
                {"duration", ticksPerSecond > 0.0 ? animation->mDuration / ticksPerSecond : animation->mDuration},
            }},
            {"runtimeSupport", decodedChannelCount > 0 ? "decoded_keyframes_runtime_playback_supported" : "decoded_keyframes_playback_system_pending"},
        });
    }
    return animations;
}

std::vector<uint32_t> fbxJointRemapForMesh(const aiMesh* mesh, const nlohmann::json& skeleton) {
    std::vector<uint32_t> remap;
    if (mesh == nullptr || !mesh->HasBones()) {
        return remap;
    }
    const std::unordered_map<std::string, uint32_t> jointIndex = fbxJointIndexByName(skeleton);
    remap.reserve(mesh->mNumBones);
    for (unsigned boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex) {
        const aiBone* bone = mesh->mBones[boneIndex];
        const std::string name = bone != nullptr ? assimpString(bone->mName) : std::string{};
        const auto it = jointIndex.find(name);
        if (it != jointIndex.end()) {
            remap.push_back(it->second);
        }
    }
    if (remap.empty()) {
        const uint32_t jointCount = static_cast<uint32_t>(skeleton.value("jointCount", 0u));
        remap.reserve(jointCount);
        for (uint32_t joint = 0; joint < jointCount; ++joint) {
            remap.push_back(joint);
        }
    }
    return remap;
}

void assignFbxMeshSkinningChannels(const aiMesh* meshSource, const nlohmann::json& skeleton, MeshAsset& mesh, nlohmann::json& skinningReport) {
    skinningReport = {
        {"skinned", false},
        {"boneCount", 0},
        {"skinnedVertexCount", 0},
        {"droppedInfluenceCount", 0},
        {"samples", nlohmann::json::array()},
    };
    if (meshSource == nullptr || !meshSource->HasBones() || mesh.vertices.empty()) {
        return;
    }
    const std::unordered_map<std::string, uint32_t> jointIndex = fbxJointIndexByName(skeleton);
    uint32_t droppedInfluenceCount = 0;
    for (unsigned boneIndex = 0; boneIndex < meshSource->mNumBones; ++boneIndex) {
        const aiBone* bone = meshSource->mBones[boneIndex];
        if (bone == nullptr) {
            continue;
        }
        const std::string boneName = assimpString(bone->mName);
        const auto jointIt = jointIndex.find(boneName);
        if (jointIt == jointIndex.end()) {
            continue;
        }
        const uint32_t joint = jointIt->second;
        for (unsigned weightIndex = 0; weightIndex < bone->mNumWeights; ++weightIndex) {
            const aiVertexWeight& weight = bone->mWeights[weightIndex];
            if (weight.mVertexId >= mesh.vertices.size() || weight.mWeight <= 0.0f) {
                continue;
            }
            MeshVertex& vertex = mesh.vertices[weight.mVertexId];
            int slot = -1;
            for (int candidate = 0; candidate < 4; ++candidate) {
                if (vertex.weights[candidate] <= 0.0f) {
                    slot = candidate;
                    break;
                }
            }
            if (slot < 0) {
                slot = 0;
                for (int candidate = 1; candidate < 4; ++candidate) {
                    if (vertex.weights[candidate] < vertex.weights[slot]) {
                        slot = candidate;
                    }
                }
                if (weight.mWeight <= vertex.weights[slot]) {
                    ++droppedInfluenceCount;
                    continue;
                }
                ++droppedInfluenceCount;
            }
            vertex.joints[slot] = joint;
            vertex.weights[slot] = weight.mWeight;
        }
    }

    uint32_t skinnedVertexCount = 0;
    nlohmann::json samples = nlohmann::json::array();
    for (size_t vertexIndex = 0; vertexIndex < mesh.vertices.size(); ++vertexIndex) {
        MeshVertex& vertex = mesh.vertices[vertexIndex];
        const float sum = vertex.weights.x + vertex.weights.y + vertex.weights.z + vertex.weights.w;
        if (sum <= 0.0f) {
            continue;
        }
        vertex.weights /= sum;
        ++skinnedVertexCount;
        if (samples.size() < 8) {
            samples.push_back({
                {"vertex", vertexIndex},
                {"joints", nlohmann::json::array({vertex.joints.x, vertex.joints.y, vertex.joints.z, vertex.joints.w})},
                {"weights", nlohmann::json::array({vertex.weights.x, vertex.weights.y, vertex.weights.z, vertex.weights.w})},
            });
        }
    }
    skinningReport = {
        {"skinned", skinnedVertexCount > 0},
        {"boneCount", meshSource->mNumBones},
        {"skinnedVertexCount", skinnedVertexCount},
        {"droppedInfluenceCount", droppedInfluenceCount},
        {"samples", samples},
    };
}
#endif

FbxStaticImportData loadFbxStaticScene(
    const std::filesystem::path& sourcePath,
    std::string_view displayName,
    const NativeTextureFormatSupport& textureFormatSupport,
    float importEmissiveScale = 1.0f) {
    FbxStaticImportData out;
#if RTV_ENABLE_ASSIMP_IMPORTER && RTV_ASSIMP_IMPORTER_AVAILABLE
    out.supported = true;
    Assimp::Importer importer;
    const unsigned flags = aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ImproveCacheLocality |
        aiProcess_SortByPType;
    const aiScene* imported = importer.ReadFile(sourcePath.string(), flags);
    if (imported == nullptr || imported->mRootNode == nullptr) {
        const char* error = importer.GetErrorString();
        out.errors.push_back(error != nullptr && error[0] != '\0' ? std::string(error) : std::string("Assimp failed to parse FBX source."));
        return out;
    }
    if ((imported->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0) {
        out.warnings.push_back("Assimp marked the FBX scene incomplete; imported static payloads may be partial.");
    }
    if (imported->HasAnimations()) {
        out.warnings.push_back("FBX animations were detected and converted to decoded .rtanim keyframes for the existing runtime playback path.");
    }
    if (imported->HasMeshes()) {
        for (unsigned meshIndex = 0; meshIndex < imported->mNumMeshes; ++meshIndex) {
            const aiMesh* meshSource = imported->mMeshes[meshIndex];
            if (meshSource != nullptr && meshSource->HasBones()) {
                out.warnings.push_back("FBX mesh '" + assimpString(meshSource->mName) + "' has bones; skeleton metadata and runtime GPU skinning bindings are preserved when Assimp exposes the skin data.");
            }
        }
    }
    out.skeletons = fbxSkeletonMetadataFromAssimp(imported);
    out.animations = fbxAnimationMetadataFromAssimp(imported);

    out.scene.name = displayName.empty() ? sourcePath.stem().string() : std::string(displayName);
    out.scene.sourcePath = sourcePath;
    const FbxTextureConvention textureConvention = detectFbxTextureConvention(sourcePath);
    const FbxPysceneOverrides pysceneOverrides = parseFbxPysceneOverrides(sourcePath);
    const bool importEmissiveScaleRequested = importEmissiveScale != 1.0f;
    const bool importEmissiveScaleApplies = importEmissiveScaleRequested && !pysceneOverrides.found;
    if (pysceneOverrides.found) {
        out.warnings.insert(out.warnings.end(), pysceneOverrides.warnings.begin(), pysceneOverrides.warnings.end());
        if (importEmissiveScaleRequested) {
            out.warnings.push_back(
                "FBX import emissiveScale was ignored because a .pyscene sidecar is present; .pyscene emissive overrides take precedence.");
        }
    }

    std::vector<MaterialAssetHandle> materialHandles;
    materialHandles.reserve(imported->mNumMaterials);
    std::unordered_map<std::string, TextureAssetHandle> textureHandlesByKey;
    nlohmann::json textureReports = nlohmann::json::array();
    nlohmann::json materialReports = nlohmann::json::array();
    for (unsigned materialIndex = 0; materialIndex < imported->mNumMaterials; ++materialIndex) {
        const aiMaterial* materialSource = imported->mMaterials[materialIndex];
        MaterialAsset material;
        material.name = "Material_" + std::to_string(materialIndex);
        if (materialSource != nullptr) {
            aiString name;
            if (materialSource->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0) {
                material.name = assimpString(name);
            }
            aiColor3D diffuse(1.0f, 1.0f, 1.0f);
            if (materialSource->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
                material.baseColorFactor = glm::vec4(assimpColor3(diffuse), 1.0f);
            }
            aiColor3D emissive(0.0f, 0.0f, 0.0f);
            if (materialSource->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
                material.emissiveFactor = assimpColor3(emissive);
            }
            aiColor3D specular(1.0f, 1.0f, 1.0f);
            if (materialSource->Get(AI_MATKEY_COLOR_SPECULAR, specular) == AI_SUCCESS) {
                material.hasSpecular = 1u;
                material.specularColorFactor = assimpColor3(specular);
                material.specularFactor = std::max({material.specularColorFactor.x, material.specularColorFactor.y, material.specularColorFactor.z});
            }
            bool scalarOpacityIsTransparent = false;
            float opacity = 1.0f;
            if (materialSource->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
                material.baseColorFactor.a = std::clamp(opacity, 0.0f, 1.0f);
                scalarOpacityIsTransparent = opacity < 0.999f;
                material.alphaMode = scalarOpacityIsTransparent ? kMaterialAlphaModeBlend : kMaterialAlphaModeOpaque;
            }
            int twoSided = 0;
            if (materialSource->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS && twoSided != 0) {
                material.doubleSided = 1u;
            }
            if (fbxMaterialNameImpliesDoubleSided(material.name)) {
                material.doubleSided = 1u;
            }
            float shininess = 0.0f;
            if (materialSource->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.0f) {
                material.roughnessFactor = std::clamp(std::sqrt(2.0f / (shininess + 2.0f)), 0.04f, 1.0f);
            }

            auto bindTexture = [&](
                aiTextureType type,
                NativeTextureRole role,
                const char* slotName,
                TextureAssetHandle MaterialAsset::*member,
                TextureTransformAsset MaterialAsset::*transformMember,
                std::optional<NativeTextureColorSpace> colorSpaceOverride = std::nullopt) -> TextureAssetHandle {
                const unsigned count = materialSource->GetTextureCount(type);
                if (count == 0) {
                    return {};
                }
                if (count > 1) {
                    out.warnings.push_back("FBX material '" + material.name + "' has multiple " + slotName + " textures; only the first texture is bound by this slice.");
                }
                aiString textureName;
                aiTextureMapping mapping = aiTextureMapping_UV;
                unsigned int uvIndex = 0;
                ai_real blend = 1.0f;
                aiTextureOp op = aiTextureOp_Multiply;
                aiTextureMapMode mapModes[3] = {aiTextureMapMode_Wrap, aiTextureMapMode_Wrap, aiTextureMapMode_Wrap};
                if (materialSource->GetTexture(type, 0, &textureName, &mapping, &uvIndex, &blend, &op, mapModes) != AI_SUCCESS || textureName.length == 0) {
                    textureReports.push_back({
                        {"materialIndex", materialIndex},
                        {"material", material.name},
                        {"slot", slotName},
                        {"assimpType", assimpTextureTypeName(type)},
                        {"status", "unreadable-reference"},
                    });
                    return {};
                }
                const TextureTransformAsset textureTransform = fbxTextureTransformFromAssimp(materialSource, type, 0, uvIndex, material.name, slotName, out.warnings);
                if (transformMember != nullptr) {
                    material.*transformMember = textureTransform;
                }

                const std::string token = assimpString(textureName);
                const NativeTextureColorSpace colorSpace = colorSpaceOverride.value_or(fbxTextureColorSpaceForRole(role));
                const aiTexture* embeddedTexture = imported->GetEmbeddedTexture(token.c_str());
                if (embeddedTexture != nullptr) {
                    const std::string key = nativeTextureRoleName(role) + std::string(":embedded:") + token;
                    const auto existing = textureHandlesByKey.find(key);
                    if (existing != textureHandlesByKey.end()) {
                        material.*member = existing->second;
                        textureReports.push_back({
                            {"materialIndex", materialIndex},
                            {"material", material.name},
                            {"slot", slotName},
                            {"role", nativeTextureRoleName(role)},
                            {"assimpType", assimpTextureTypeName(type)},
                            {"token", token},
                            {"uvIndex", uvIndex},
                            {"mapModeU", assimpTextureMapModeName(mapModes[0])},
                            {"mapModeV", assimpTextureMapModeName(mapModes[1])},
                            {"textureTransform", textureTransformJson(textureTransform)},
                            {"textureIndex", existing->second.index},
                            {"source", "embedded"},
                            {"status", "reused"},
                        });
                        return existing->second;
                    }

                    try {
                        std::string embeddedStorage;
                        const TextureData textureData = loadEmbeddedFbxTextureData(embeddedTexture, textureFormatSupport, role, colorSpace, embeddedStorage);
                        TextureAsset texture;
                        texture.name = embeddedFbxTextureName(sourcePath, token, role);
                        texture.sourcePath = std::filesystem::path(sourcePath.filename().string() + "#" + token);
                        texture.width = static_cast<uint32_t>(std::max(0, textureData.width));
                        texture.height = static_cast<uint32_t>(std::max(0, textureData.height));
                        texture.channels = 4;
                        texture.sourceArrayLayers = textureData.sourceArrayLayers;
                        texture.sourceDepth = textureData.sourceDepth;
                        texture.sourceFaceCount = textureData.sourceFaceCount;
                        texture.sourceIsCubemap = textureData.sourceIsCubemap;
                        texture.mipLevels = std::max(1, textureData.mipLevels);
                        texture.srgb = colorSpace == NativeTextureColorSpace::Srgb;
                        texture.linearColorSpace = colorSpace != NativeTextureColorSpace::Srgb || textureData.linearColorSpace;
                        texture.isCompressed = textureData.isCompressed;
                        texture.format = textureData.format;
                        texture.compressedFormat = textureData.compressedFormat;
                        texture.sourceContainerKind = textureData.sourceContainerKind;
                        texture.nativePayloadSource = textureData.nativePayloadSource;
                        texture.sourceContainerPreserved = textureData.sourceContainerPreserved;
                        texture.sourceContainerTranscoded = textureData.sourceContainerTranscoded;
                        texture.rgba8 = textureData.pixels;
                        texture.mipData = textureData.mipData;
                        applyFbxTextureSampler(texture, mapModes);
                        const TextureAssetHandle handle = out.assets.addTexture(std::move(texture));
                        if (handle.index >= out.textureRoles.size()) {
                            out.textureRoles.resize(static_cast<size_t>(handle.index) + 1u);
                        }
                        out.textureRoles[handle.index] = nativeTextureRoleName(role);
                        textureHandlesByKey.emplace(key, handle);
                        material.*member = handle;
                        textureReports.push_back({
                            {"materialIndex", materialIndex},
                            {"material", material.name},
                            {"slot", slotName},
                            {"role", nativeTextureRoleName(role)},
                            {"colorSpace", nativeTextureColorSpaceName(colorSpace)},
                            {"assimpType", assimpTextureTypeName(type)},
                            {"token", token},
                            {"uvIndex", uvIndex},
                            {"mapModeU", assimpTextureMapModeName(mapModes[0])},
                            {"mapModeV", assimpTextureMapModeName(mapModes[1])},
                            {"textureTransform", textureTransformJson(textureTransform)},
                            {"source", "embedded"},
                            {"storage", embeddedStorage},
                            {"formatHint", embeddedFbxTextureFormatHint(embeddedTexture)},
                            {"textureIndex", handle.index},
                            {"width", textureData.width},
                            {"height", textureData.height},
                            {"status", "decoded"},
                        });
                        return handle;
                    } catch (const std::exception& ex) {
                        textureReports.push_back({
                            {"materialIndex", materialIndex},
                            {"material", material.name},
                            {"slot", slotName},
                            {"role", nativeTextureRoleName(role)},
                            {"assimpType", assimpTextureTypeName(type)},
                            {"token", token},
                            {"source", "embedded"},
                            {"status", "decode-failed"},
                            {"reason", ex.what()},
                        });
                        out.warnings.push_back("FBX embedded texture decode failed for material '" + material.name + "': " + token + " " + ex.what());
                    }
                    return {};
                }

                std::string resolveReason;
                const std::optional<std::filesystem::path> resolvedPath = resolveFbxTexturePath(sourcePath, token, resolveReason);
                if (!resolvedPath.has_value()) {
                    textureReports.push_back({
                        {"materialIndex", materialIndex},
                        {"material", material.name},
                        {"slot", slotName},
                        {"role", nativeTextureRoleName(role)},
                        {"assimpType", assimpTextureTypeName(type)},
                        {"token", token},
                        {"uvIndex", uvIndex},
                        {"mapModeU", assimpTextureMapModeName(mapModes[0])},
                        {"mapModeV", assimpTextureMapModeName(mapModes[1])},
                        {"textureTransform", textureTransformJson(textureTransform)},
                        {"status", "unresolved"},
                        {"reason", resolveReason},
                    });
                    out.warnings.push_back("FBX texture reference for material '" + material.name + "' did not resolve: " + token + " (" + resolveReason + ")");
                    return {};
                }

                const std::string key = nativeTextureRoleName(role) + std::string(":") + resolvedPath->generic_string();
                const auto existing = textureHandlesByKey.find(key);
                if (existing != textureHandlesByKey.end()) {
                    material.*member = existing->second;
                    textureReports.push_back({
                        {"materialIndex", materialIndex},
                        {"material", material.name},
                        {"slot", slotName},
                        {"role", nativeTextureRoleName(role)},
                        {"assimpType", assimpTextureTypeName(type)},
                        {"token", token},
                        {"resolvedPath", resolvedPath->generic_string()},
                        {"uvIndex", uvIndex},
                        {"mapModeU", assimpTextureMapModeName(mapModes[0])},
                        {"mapModeV", assimpTextureMapModeName(mapModes[1])},
                        {"textureTransform", textureTransformJson(textureTransform)},
                        {"textureIndex", existing->second.index},
                        {"status", "reused"},
                    });
                    return existing->second;
                }

                try {
                    const TextureData textureData = TextureLoader::load(resolvedPath->string(), textureFormatSupport, role, colorSpace);
                    TextureAsset texture;
                    texture.name = safeStem(resolvedPath->stem().string() + "_" + nativeTextureRoleName(role));
                    texture.sourcePath = *resolvedPath;
                    texture.width = static_cast<uint32_t>(std::max(0, textureData.width));
                    texture.height = static_cast<uint32_t>(std::max(0, textureData.height));
                    texture.channels = 4;
                    texture.sourceArrayLayers = textureData.sourceArrayLayers;
                    texture.sourceDepth = textureData.sourceDepth;
                    texture.sourceFaceCount = textureData.sourceFaceCount;
                    texture.sourceIsCubemap = textureData.sourceIsCubemap;
                    texture.mipLevels = std::max(1, textureData.mipLevels);
                    texture.srgb = colorSpace == NativeTextureColorSpace::Srgb;
                    texture.linearColorSpace = colorSpace != NativeTextureColorSpace::Srgb || textureData.linearColorSpace;
                    texture.isCompressed = textureData.isCompressed;
                    texture.format = textureData.format;
                    texture.compressedFormat = textureData.compressedFormat;
                    texture.sourceContainerKind = textureData.sourceContainerKind;
                    texture.nativePayloadSource = textureData.nativePayloadSource;
                    texture.sourceContainerPreserved = textureData.sourceContainerPreserved;
                    texture.sourceContainerTranscoded = textureData.sourceContainerTranscoded;
                    texture.rgba8 = textureData.pixels;
                    texture.mipData = textureData.mipData;
                    applyFbxTextureSampler(texture, mapModes);
                    const TextureAssetHandle handle = out.assets.addTexture(std::move(texture));
                    if (handle.index >= out.textureRoles.size()) {
                        out.textureRoles.resize(static_cast<size_t>(handle.index) + 1u);
                    }
                    out.textureRoles[handle.index] = nativeTextureRoleName(role);
                    textureHandlesByKey.emplace(key, handle);
                    material.*member = handle;
                    textureReports.push_back({
                        {"materialIndex", materialIndex},
                        {"material", material.name},
                        {"slot", slotName},
                        {"role", nativeTextureRoleName(role)},
                        {"colorSpace", nativeTextureColorSpaceName(colorSpace)},
                        {"assimpType", assimpTextureTypeName(type)},
                        {"token", token},
                        {"resolvedPath", resolvedPath->generic_string()},
                        {"uvIndex", uvIndex},
                        {"mapModeU", assimpTextureMapModeName(mapModes[0])},
                        {"mapModeV", assimpTextureMapModeName(mapModes[1])},
                        {"textureTransform", textureTransformJson(textureTransform)},
                        {"textureIndex", handle.index},
                        {"width", textureData.width},
                        {"height", textureData.height},
                        {"status", "decoded"},
                    });
                    return handle;
                } catch (const std::exception& ex) {
                    textureReports.push_back({
                        {"materialIndex", materialIndex},
                        {"material", material.name},
                        {"slot", slotName},
                        {"role", nativeTextureRoleName(role)},
                        {"assimpType", assimpTextureTypeName(type)},
                        {"token", token},
                        {"resolvedPath", resolvedPath->generic_string()},
                        {"uvIndex", uvIndex},
                        {"mapModeU", assimpTextureMapModeName(mapModes[0])},
                        {"mapModeV", assimpTextureMapModeName(mapModes[1])},
                        {"textureTransform", textureTransformJson(textureTransform)},
                        {"status", "decode-failed"},
                        {"reason", ex.what()},
                    });
                    out.warnings.push_back("FBX texture decode failed for material '" + material.name + "': " + resolvedPath->string() + " " + ex.what());
                }
                return {};
            };

            TextureAssetHandle baseColorHandle = bindTexture(aiTextureType_BASE_COLOR, NativeTextureRole::BaseColor, "baseColor", &MaterialAsset::baseColorTexture, &MaterialAsset::baseColorTextureTransform);
            if (!material.baseColorTexture.valid()) {
                baseColorHandle = bindTexture(aiTextureType_DIFFUSE, NativeTextureRole::BaseColor, "baseColor", &MaterialAsset::baseColorTexture, &MaterialAsset::baseColorTextureTransform);
            } else if (!baseColorHandle.valid()) {
                baseColorHandle = material.baseColorTexture;
            }
            bindTexture(aiTextureType_EMISSION_COLOR, NativeTextureRole::Emissive, "emissive", &MaterialAsset::emissiveTexture, &MaterialAsset::emissiveTextureTransform);
            if (!material.emissiveTexture.valid()) {
                bindTexture(aiTextureType_EMISSIVE, NativeTextureRole::Emissive, "emissive", &MaterialAsset::emissiveTexture, &MaterialAsset::emissiveTextureTransform);
            }
            bindTexture(aiTextureType_NORMAL_CAMERA, NativeTextureRole::Normal, "normal", &MaterialAsset::normalTexture, &MaterialAsset::normalTextureTransform);
            if (!material.normalTexture.valid()) {
                bindTexture(aiTextureType_NORMALS, NativeTextureRole::Normal, "normal", &MaterialAsset::normalTexture, &MaterialAsset::normalTextureTransform);
            }
            if (!material.normalTexture.valid()) {
                bindTexture(aiTextureType_HEIGHT, NativeTextureRole::Normal, "normal", &MaterialAsset::normalTexture, &MaterialAsset::normalTextureTransform);
            } else {
                bindTexture(aiTextureType_HEIGHT, NativeTextureRole::Height, "height", &MaterialAsset::heightTexture, nullptr);
            }
            bindTexture(aiTextureType_OPACITY, NativeTextureRole::Opacity, "opacity", &MaterialAsset::opacityTexture, nullptr, NativeTextureColorSpace::Linear);
            bindTexture(aiTextureType_METALNESS, NativeTextureRole::Metallic, "metallicRoughness", &MaterialAsset::metallicRoughnessTexture, &MaterialAsset::metallicRoughnessTextureTransform);
            if (!material.metallicRoughnessTexture.valid()) {
                bindTexture(aiTextureType_DIFFUSE_ROUGHNESS, NativeTextureRole::Roughness, "metallicRoughness", &MaterialAsset::metallicRoughnessTexture, &MaterialAsset::metallicRoughnessTextureTransform);
            }
            bindTexture(aiTextureType_AMBIENT_OCCLUSION, NativeTextureRole::Occlusion, "occlusion", &MaterialAsset::occlusionTexture, &MaterialAsset::occlusionTextureTransform);
            if (!material.occlusionTexture.valid()) {
                bindTexture(aiTextureType_LIGHTMAP, NativeTextureRole::Occlusion, "occlusion", &MaterialAsset::occlusionTexture, &MaterialAsset::occlusionTextureTransform);
            }
            if (textureConvention.specularIsPackedOcclusionRoughnessMetalness) {
                TextureAssetHandle packedOrmHandle = bindTexture(
                    aiTextureType_SPECULAR,
                    NativeTextureRole::MetallicRoughness,
                    "packedOcclusionRoughnessMetalness",
                    &MaterialAsset::metallicRoughnessTexture,
                    &MaterialAsset::metallicRoughnessTextureTransform,
                    NativeTextureColorSpace::Linear);
                if (packedOrmHandle.valid()) {
                    material.occlusionTexture = packedOrmHandle;
                    material.occlusionTextureTransform = material.metallicRoughnessTextureTransform;
                    material.materialWorkflow = kMaterialWorkflowPackedOcclusionRoughnessMetalness;
                    material.metallicFactor = 1.0f;
                    material.roughnessFactor = 1.0f;
                    material.hasSpecular = 0u;
                    material.specularFactor = 1.0f;
                    material.specularColorFactor = glm::vec3{1.0f};
                    material.specularTexture = {};
                    material.specularColorTexture = {};
                }
            } else {
                TextureAssetHandle specularHandle = bindTexture(aiTextureType_SPECULAR, NativeTextureRole::Data, "specular", &MaterialAsset::specularTexture, &MaterialAsset::specularTextureTransform);
                if (specularHandle.valid()) {
                    material.hasSpecular = 1u;
                    material.materialWorkflow = kMaterialWorkflowSpecularGlossiness;
                    material.specularColorTexture = specularHandle;
                    material.specularColorTextureTransform = material.specularTextureTransform;
                    if (material.specularFactor <= 0.0f) {
                        material.specularFactor = 1.0f;
                    }
                }
            }

            if (material.normalTexture.valid()) {
                const TextureAsset* normalTexture = out.assets.texture(material.normalTexture);
                if (textureConvention.normalMapIsDirectX || (normalTexture != nullptr && textureAssetSourceLooksDds(*normalTexture))) {
                    material.normalMapConvention = kMaterialNormalMapDirectX;
                }
            }
            if (baseColorHandle.valid()) {
                if (const TextureAsset* baseTexture = out.assets.texture(baseColorHandle)) {
                    const bool baseAlphaIsOpacity = textureConvention.baseColorAlphaIsOpacity || textureAssetHasNonOpaqueAlpha(*baseTexture);
                    if (baseAlphaIsOpacity && textureAssetHasNonOpaqueAlpha(*baseTexture)) {
                        material.alphaMode = (scalarOpacityIsTransparent || fbxMaterialNameImpliesAlphaBlend(material.name)) ? kMaterialAlphaModeBlend : kMaterialAlphaModeMask;
                        material.alphaCutoff = 0.5f;
                        if (material.alphaMode == kMaterialAlphaModeMask && fbxMaterialNameImpliesDoubleSided(material.name)) {
                            material.doubleSided = 1u;
                        }
                    }
                }
            }
            if (material.opacityTexture.valid() && material.alphaMode == kMaterialAlphaModeOpaque) {
                material.alphaMode = fbxMaterialNameImpliesAlphaBlend(material.name) ? kMaterialAlphaModeBlend : kMaterialAlphaModeMask;
                material.alphaCutoff = 0.5f;
            }
        }
        if (pysceneOverrides.globalEmissiveMultiplier != 1.0f) {
            material.emissiveFactor *= pysceneOverrides.globalEmissiveMultiplier;
            material.hasEmissiveStrength = 1u;
        }
        // Optional source-package emissive scaling import setting (FBX has no standard emissive
        // strength unit, so this lets the importer normalize authored emissive intensity).
        if (importEmissiveScaleApplies) {
            material.emissiveFactor *= importEmissiveScale;
            material.hasEmissiveStrength = 1u;
        }
        nlohmann::json appliedPysceneOverrides = nlohmann::json::array();
        const auto overrideIt = pysceneOverrides.materials.find(material.name);
        if (overrideIt != pysceneOverrides.materials.end()) {
            applyFbxPysceneMaterialOverride(material, overrideIt->second);
            const FbxPysceneMaterialOverride& overrideData = overrideIt->second;
            if (overrideData.roughness.has_value()) appliedPysceneOverrides.push_back("roughness");
            if (overrideData.metallic.has_value()) appliedPysceneOverrides.push_back("metallic");
            if (overrideData.indexOfRefraction.has_value()) appliedPysceneOverrides.push_back("indexOfRefraction");
            if (overrideData.specularTransmission.has_value()) appliedPysceneOverrides.push_back("specularTransmission");
            if (overrideData.doubleSided.has_value()) appliedPysceneOverrides.push_back("doubleSided");
            if (overrideData.nestedPriority.has_value()) appliedPysceneOverrides.push_back("nestedPriority");
            if (overrideData.volumeAbsorption.has_value()) appliedPysceneOverrides.push_back("volumeAbsorption");
            if (overrideData.emissiveFactor.has_value()) appliedPysceneOverrides.push_back("emissiveFactor");
            if (overrideData.emissiveMultiplier != 1.0f) appliedPysceneOverrides.push_back("emissiveFactorMultiplier");
        }
        materialHandles.push_back(out.assets.addMaterial(material));
        out.scene.materials.push_back(materialHandles.back());
        materialReports.push_back({
            {"index", materialIndex},
            {"name", material.name},
            {"alphaMode", material.alphaMode},
            {"alphaModeLabel", materialAlphaModeLabel(material.alphaMode)},
            {"alphaCutoff", material.alphaCutoff},
            {"doubleSided", material.doubleSided != 0u},
            {"workflow", materialWorkflowLabel(material.materialWorkflow)},
            {"normalMapConvention", materialNormalMapConventionLabel(material.normalMapConvention)},
            {"roughnessFactor", material.roughnessFactor},
            {"metallicFactor", material.metallicFactor},
            {"baseColorTexture", material.baseColorTexture.valid() ? static_cast<int>(material.baseColorTexture.index) : -1},
            {"normalTexture", material.normalTexture.valid() ? static_cast<int>(material.normalTexture.index) : -1},
            {"metallicRoughnessTexture", material.metallicRoughnessTexture.valid() ? static_cast<int>(material.metallicRoughnessTexture.index) : -1},
            {"emissiveTexture", material.emissiveTexture.valid() ? static_cast<int>(material.emissiveTexture.index) : -1},
            {"occlusionTexture", material.occlusionTexture.valid() ? static_cast<int>(material.occlusionTexture.index) : -1},
            {"opacityTexture", material.opacityTexture.valid() ? static_cast<int>(material.opacityTexture.index) : -1},
            {"specularTexture", material.specularTexture.valid() ? static_cast<int>(material.specularTexture.index) : -1},
            {"specularColorTexture", material.specularColorTexture.valid() ? static_cast<int>(material.specularColorTexture.index) : -1},
            {"emissiveFactor", vec3ArrayJson(material.emissiveFactor)},
            {"transmissionFactor", material.transmissionFactor},
            {"iorFactor", material.iorFactor},
            {"hasVolume", material.hasVolume != 0u},
            {"volumeAttenuationColor", vec3ArrayJson(material.volumeAttenuationColor)},
            {"nestedPriority", material.nestedPriority},
            {"pysceneOverrides", appliedPysceneOverrides},
        });
    }

    nlohmann::json meshReports = nlohmann::json::array();
    for (unsigned meshIndex = 0; meshIndex < imported->mNumMeshes; ++meshIndex) {
        const aiMesh* meshSource = imported->mMeshes[meshIndex];
        if (meshSource == nullptr || !meshSource->HasPositions() || meshSource->mNumFaces == 0) {
            continue;
        }
        MeshAsset mesh;
        mesh.name = meshSource->mName.length > 0 ? assimpString(meshSource->mName) : "FbxMesh_" + std::to_string(meshIndex);
        const MaterialAsset* meshMaterial = meshSource->mMaterialIndex < materialHandles.size()
            ? out.assets.material(materialHandles[meshSource->mMaterialIndex])
            : nullptr;
        const bool ignoreVertexColors = meshSource->HasVertexColors(0) &&
            meshMaterial != nullptr &&
            fbxMaterialVertexColorsAreBlendMasks(meshMaterial->name);
        mesh.vertices.reserve(meshSource->mNumVertices);
        for (unsigned vertexIndex = 0; vertexIndex < meshSource->mNumVertices; ++vertexIndex) {
            MeshVertex vertex;
            const aiVector3D& position = meshSource->mVertices[vertexIndex];
            vertex.position = glm::vec3{position.x, position.y, position.z};
            if (meshSource->HasNormals()) {
                const aiVector3D& normal = meshSource->mNormals[vertexIndex];
                const glm::vec3 n{normal.x, normal.y, normal.z};
                const float len2 = glm::dot(n, n);
                vertex.normal = len2 > 1.0e-12f ? n / std::sqrt(len2) : glm::vec3{0.0f, 1.0f, 0.0f};
            }
            if (meshSource->HasTangentsAndBitangents()) {
                const aiVector3D& tangent = meshSource->mTangents[vertexIndex];
                const aiVector3D& bitangent = meshSource->mBitangents[vertexIndex];
                glm::vec3 t{tangent.x, tangent.y, tangent.z};
                const glm::vec3 b{bitangent.x, bitangent.y, bitangent.z};
                t = t - vertex.normal * glm::dot(vertex.normal, t);
                const float tangentLen2 = glm::dot(t, t);
                if (tangentLen2 > 1.0e-12f) {
                    t /= std::sqrt(tangentLen2);
                } else {
                    t = glm::vec3{1.0f, 0.0f, 0.0f};
                }
                const float handedness = glm::dot(glm::cross(vertex.normal, t), b) < 0.0f ? -1.0f : 1.0f;
                vertex.tangent = glm::vec4{t, handedness};
            }
            if (meshSource->HasTextureCoords(0)) {
                const aiVector3D& uv = meshSource->mTextureCoords[0][vertexIndex];
                vertex.texcoord = glm::vec2{uv.x, 1.0f - uv.y};
            }
            if (meshSource->HasTextureCoords(1)) {
                const aiVector3D& uv = meshSource->mTextureCoords[1][vertexIndex];
                vertex.texcoord1 = glm::vec2{uv.x, 1.0f - uv.y};
            }
            if (meshSource->HasVertexColors(0) && !ignoreVertexColors) {
                const aiColor4D& color = meshSource->mColors[0][vertexIndex];
                vertex.color = glm::vec4{color.r, color.g, color.b, color.a};
            }
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.reserve(meshSource->mNumFaces * 3u);
        size_t triangleFaceCount = 0;
        size_t nonTriangleFaceCount = 0;
        for (unsigned faceIndex = 0; faceIndex < meshSource->mNumFaces; ++faceIndex) {
            const aiFace& face = meshSource->mFaces[faceIndex];
            if (face.mNumIndices != 3) {
                ++nonTriangleFaceCount;
                continue;
            }
            ++triangleFaceCount;
            mesh.indices.push_back(face.mIndices[0]);
            mesh.indices.push_back(face.mIndices[1]);
            mesh.indices.push_back(face.mIndices[2]);
        }
        nlohmann::json skinningReport = nlohmann::json::object();
        if (!out.skeletons.empty()) {
            assignFbxMeshSkinningChannels(meshSource, out.skeletons.front(), mesh, skinningReport);
        }
        MeshPrimitiveAsset primitive;
        primitive.firstVertex = 0;
        primitive.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        primitive.firstIndex = 0;
        primitive.indexCount = static_cast<uint32_t>(mesh.indices.size());
        if (meshSource->mMaterialIndex < materialHandles.size()) {
            primitive.material = materialHandles[meshSource->mMaterialIndex];
        }
        updatePrimitiveAlphaClassification(primitive, out.assets.material(primitive.material));
        // FBX morph/blend-shape channels arrive as Assimp anim meshes that store
        // absolute deformed positions. Convert them to the engine's delta-based
        // MorphTarget layout (delta = deformed - base) so they drive the existing
        // GPU morph runtime, animation morph weights, and inspector sliders.
        nlohmann::json morphTargetReports = nlohmann::json::array();
        size_t morphPositionDeltaCount = 0;
        size_t morphNormalDeltaCount = 0;
        for (unsigned animMeshIndex = 0; animMeshIndex < meshSource->mNumAnimMeshes; ++animMeshIndex) {
            const aiAnimMesh* animMesh = meshSource->mAnimMeshes[animMeshIndex];
            if (animMesh == nullptr || animMesh->mNumVertices != meshSource->mNumVertices) {
                continue;
            }
            MeshPrimitiveAsset::MorphTarget target;
            target.name = animMesh->mName.length > 0
                ? assimpString(animMesh->mName)
                : "MorphTarget_" + std::to_string(animMeshIndex);
            bool anyNonZeroPosition = false;
            if (animMesh->HasPositions()) {
                target.positionDeltas.resize(meshSource->mNumVertices, glm::vec3{0.0f});
                for (unsigned vertexIndex = 0; vertexIndex < meshSource->mNumVertices; ++vertexIndex) {
                    const aiVector3D& deformed = animMesh->mVertices[vertexIndex];
                    const aiVector3D& base = meshSource->mVertices[vertexIndex];
                    const glm::vec3 delta{deformed.x - base.x, deformed.y - base.y, deformed.z - base.z};
                    target.positionDeltas[vertexIndex] = delta;
                    if (glm::dot(delta, delta) > 1.0e-12f) {
                        anyNonZeroPosition = true;
                    }
                }
            }
            if (animMesh->HasNormals() && meshSource->HasNormals()) {
                target.normalDeltas.resize(meshSource->mNumVertices, glm::vec3{0.0f});
                for (unsigned vertexIndex = 0; vertexIndex < meshSource->mNumVertices; ++vertexIndex) {
                    const aiVector3D& deformed = animMesh->mNormals[vertexIndex];
                    const aiVector3D& base = meshSource->mNormals[vertexIndex];
                    target.normalDeltas[vertexIndex] =
                        glm::vec3{deformed.x - base.x, deformed.y - base.y, deformed.z - base.z};
                }
            }
            if (animMesh->HasTangentsAndBitangents() && meshSource->HasTangentsAndBitangents()) {
                target.tangentDeltas.resize(meshSource->mNumVertices, glm::vec3{0.0f});
                for (unsigned vertexIndex = 0; vertexIndex < meshSource->mNumVertices; ++vertexIndex) {
                    const aiVector3D& deformed = animMesh->mTangents[vertexIndex];
                    const aiVector3D& base = meshSource->mTangents[vertexIndex];
                    target.tangentDeltas[vertexIndex] =
                        glm::vec3{deformed.x - base.x, deformed.y - base.y, deformed.z - base.z};
                }
            }
            if (target.positionDeltas.empty() && target.normalDeltas.empty() && target.tangentDeltas.empty()) {
                continue;
            }
            morphPositionDeltaCount += target.positionDeltas.size();
            morphNormalDeltaCount += target.normalDeltas.size();
            morphTargetReports.push_back({
                {"index", animMeshIndex},
                {"name", target.name},
                {"defaultWeight", animMesh->mWeight},
                {"hasPositionDeltas", !target.positionDeltas.empty()},
                {"hasNormalDeltas", !target.normalDeltas.empty()},
                {"hasTangentDeltas", !target.tangentDeltas.empty()},
                {"nonZeroPositionDeltas", anyNonZeroPosition},
            });
            primitive.morphTargets.push_back(std::move(target));
            // Seed a neutral default weight so morph data round-trips without
            // forcing a deformed bind pose; animation channels drive it later.
            mesh.defaultMorphWeights.push_back(0.0f);
        }
        mesh.primitives.push_back(primitive);
        MeshAssetHandle handle = out.assets.addMesh(mesh);
        out.scene.meshes.push_back(handle);
        meshReports.push_back({
            {"index", meshIndex},
            {"name", mesh.name},
            {"vertexCount", mesh.vertices.size()},
            {"indexCount", mesh.indices.size()},
            {"sourceFaceCount", meshSource->mNumFaces},
            {"triangleFaceCount", triangleFaceCount},
            {"nonTriangleFaceCount", nonTriangleFaceCount},
            {"triangulation", {
                {"policy", "assimp_aiProcess_Triangulate"},
                {"postTriangulationAllFacesTriangles", nonTriangleFaceCount == 0},
                {"ngonTriangulationDiagnostics", "source_ngons_are_triangulated_by_assimp_before_runtime_cook; nonTriangleFaceCount reports any residual unsupported faces"},
            }},
            {"smoothingAndHardEdges", fbxHardEdgeDiagnostics(mesh, meshSource->HasNormals())},
            {"materialIndex", meshSource->mMaterialIndex},
            {"attributes", {
                {"positions", meshSource->HasPositions()},
                {"normals", meshSource->HasNormals()},
                {"tangents", meshSource->mTangents != nullptr},
                {"bitangents", meshSource->mBitangents != nullptr},
                {"uv0", meshSource->HasTextureCoords(0)},
                {"uv1", meshSource->HasTextureCoords(1)},
                {"colors0", meshSource->HasVertexColors(0)},
                {"bones", meshSource->HasBones()},
                {"morphTargets", meshSource->mNumAnimMeshes > 0},
            }},
            {"hasVertexColors", meshSource->HasVertexColors(0)},
            {"vertexColorPolicy", ignoreVertexColors ? "ignored_fbx_blend_mask" : (meshSource->HasVertexColors(0) ? "imported_as_color" : "none")},
            {"skinning", skinningReport},
            {"morphTargets", {
                {"sourceAnimMeshCount", meshSource->mNumAnimMeshes},
                {"importedTargetCount", morphTargetReports.size()},
                {"positionDeltaCount", morphPositionDeltaCount},
                {"normalDeltaCount", morphNormalDeltaCount},
                {"targets", morphTargetReports},
            }},
        });
    }

    std::unordered_map<const aiNode*, uint32_t> nodeIndices;
    nlohmann::json nodeReports = nlohmann::json::array();
    size_t hiddenNodeCount = 0;
    size_t nonCameraVisibleNodeCount = 0;
    size_t nonShadowCasterNodeCount = 0;
    size_t nonShadowReceiverNodeCount = 0;
    size_t renderLayerOverrideNodeCount = 0;
    size_t nodeMetadataPropertyCount = 0;
    size_t negativeScaleNodeCount = 0;
    size_t nonUniformScaleNodeCount = 0;
    size_t fbxAuthoredGeometricOpNodeCount = 0;
    std::function<void(const aiNode*, int32_t)> appendNode = [&](const aiNode* node, int32_t parent) {
        if (node == nullptr) {
            return;
        }
        const uint32_t nodeIndex = static_cast<uint32_t>(out.scene.nodes.size());
        nodeIndices[node] = nodeIndex;
        SceneNodeAsset sceneNode;
        sceneNode.name = node->mName.length > 0 ? assimpString(node->mName) : "FbxNode_" + std::to_string(nodeIndex);
        sceneNode.sourceNodeIndex = static_cast<int32_t>(nodeIndex);
        sceneNode.transform = assimpMatrixToGlm(node->mTransformation);
        sceneNode.parent = parent;
        const nlohmann::json nodeMetadata = applyFbxNodeMetadataToSceneNode(node, sceneNode);
        const nlohmann::json transformDiagnostics = fbxTransformDiagnostics(node, sceneNode.transform);
        nodeMetadataPropertyCount += nodeMetadata.value("metadataCount", static_cast<size_t>(0));
        if (node->mNumMeshes > 0) {
            const unsigned meshIndex = node->mMeshes[0];
            if (meshIndex < out.scene.meshes.size()) {
                sceneNode.mesh = out.scene.meshes[meshIndex];
                if (!out.skeletons.empty() && imported->mMeshes[meshIndex] != nullptr && imported->mMeshes[meshIndex]->HasBones()) {
                    sceneNode.skinIndex = 0;
                }
            }
            if (node->mNumMeshes > 1) {
                out.warnings.push_back("FBX node '" + sceneNode.name + "' references multiple meshes; extra mesh slots were expanded into child nodes.");
            }
        }
        if (!sceneNode.visible) ++hiddenNodeCount;
        if (!sceneNode.visibleToCamera) ++nonCameraVisibleNodeCount;
        if (!sceneNode.castShadow) ++nonShadowCasterNodeCount;
        if (!sceneNode.receiveShadow) ++nonShadowReceiverNodeCount;
        if (sceneNode.renderLayer != 0) ++renderLayerOverrideNodeCount;
        if (transformDiagnostics.value("negativeScale", false)) ++negativeScaleNodeCount;
        if (transformDiagnostics.value("nonUniformScale", false)) ++nonUniformScaleNodeCount;
        if (transformDiagnostics.value("authoredFbxOpCount", static_cast<size_t>(0)) > 0u) ++fbxAuthoredGeometricOpNodeCount;
        nodeReports.push_back({
            {"index", nodeIndex},
            {"sourceNodeIndex", sceneNode.sourceNodeIndex},
            {"name", sceneNode.name},
            {"parent", parent},
            {"childCount", node->mNumChildren},
            {"meshSlotCount", node->mNumMeshes},
            {"mesh", sceneNode.mesh.valid() ? static_cast<int>(sceneNode.mesh.index) : -1},
            {"visible", sceneNode.visible},
            {"visibleToCamera", sceneNode.visibleToCamera},
            {"castShadow", sceneNode.castShadow},
            {"receiveShadow", sceneNode.receiveShadow},
            {"renderLayer", sceneNode.renderLayer},
            {"metadata", nodeMetadata},
            {"geometricTransform", transformDiagnostics},
        });
        out.scene.nodes.push_back(sceneNode);
        if (parent >= 0 && static_cast<size_t>(parent) < out.scene.nodes.size()) {
            out.scene.nodes[static_cast<size_t>(parent)].children.push_back(nodeIndex);
        } else {
            out.scene.rootNodes.push_back(nodeIndex);
        }
        if (node->mNumMeshes > 1) {
            for (unsigned meshSlot = 1; meshSlot < node->mNumMeshes; ++meshSlot) {
                const unsigned meshIndex = node->mMeshes[meshSlot];
                if (meshIndex >= out.scene.meshes.size()) {
                    continue;
                }
                const uint32_t meshNodeIndex = static_cast<uint32_t>(out.scene.nodes.size());
                SceneNodeAsset meshNode;
                meshNode.name = sceneNode.name + "_Mesh_" + std::to_string(meshSlot);
                meshNode.sourceNodeIndex = sceneNode.sourceNodeIndex;
                meshNode.transform = glm::mat4(1.0f);
                meshNode.parent = static_cast<int32_t>(nodeIndex);
                meshNode.mesh = out.scene.meshes[meshIndex];
                meshNode.visible = sceneNode.visible;
                meshNode.visibleToCamera = sceneNode.visibleToCamera;
                meshNode.castShadow = sceneNode.castShadow;
                meshNode.receiveShadow = sceneNode.receiveShadow;
                meshNode.renderLayer = sceneNode.renderLayer;
                if (!out.skeletons.empty() && imported->mMeshes[meshIndex] != nullptr && imported->mMeshes[meshIndex]->HasBones()) {
                    meshNode.skinIndex = 0;
                }
                nodeReports.push_back({
                    {"index", meshNodeIndex},
                    {"sourceNodeIndex", meshNode.sourceNodeIndex},
                    {"name", meshNode.name},
                    {"parent", meshNode.parent},
                    {"childCount", 0},
                    {"meshSlotCount", 1},
                    {"mesh", meshNode.mesh.valid() ? static_cast<int>(meshNode.mesh.index) : -1},
                    {"expandedFromSourceNode", sceneNode.name},
                    {"expandedMeshSlot", meshSlot},
                    {"visible", meshNode.visible},
                    {"visibleToCamera", meshNode.visibleToCamera},
                    {"castShadow", meshNode.castShadow},
                    {"receiveShadow", meshNode.receiveShadow},
                    {"renderLayer", meshNode.renderLayer},
                    {"metadata", nodeMetadata},
                    {"geometricTransform", transformDiagnostics},
                });
                out.scene.nodes.push_back(std::move(meshNode));
                out.scene.nodes[static_cast<size_t>(nodeIndex)].children.push_back(meshNodeIndex);
            }
        }
        for (unsigned childIndex = 0; childIndex < node->mNumChildren; ++childIndex) {
            appendNode(node->mChildren[childIndex], static_cast<int32_t>(nodeIndex));
        }
    };
    appendNode(imported->mRootNode, -1);
    auto nodeWorldTransform = [&](size_t nodeIndex) {
        glm::mat4 world = glm::mat4(1.0f);
        int32_t cursor = static_cast<int32_t>(nodeIndex);
        std::vector<int32_t> chain;
        while (cursor >= 0 && static_cast<size_t>(cursor) < out.scene.nodes.size()) {
            chain.push_back(cursor);
            cursor = out.scene.nodes[static_cast<size_t>(cursor)].parent;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            world = world * out.scene.nodes[static_cast<size_t>(*it)].transform;
        }
        return world;
    };

    for (unsigned cameraIndex = 0; cameraIndex < imported->mNumCameras; ++cameraIndex) {
        const aiCamera* camera = imported->mCameras[cameraIndex];
        if (camera == nullptr) {
            continue;
        }
        const std::string name = assimpString(camera->mName);
        for (SceneNodeAsset& node : out.scene.nodes) {
            if (node.name == name) {
                node.hasCamera = true;
                node.cameraProjection = 0;
                const float aspect = camera->mAspect > 0.0f ? camera->mAspect : (16.0f / 9.0f);
                node.cameraYfov = 2.0f * std::atan(std::tan(camera->mHorizontalFOV * 0.5f) / aspect);
                node.cameraAspectRatio = aspect;
                node.cameraNear = camera->mClipPlaneNear;
                node.cameraFar = camera->mClipPlaneFar;
                break;
            }
        }
    }

    for (unsigned lightIndex = 0; lightIndex < imported->mNumLights; ++lightIndex) {
        const aiLight* light = imported->mLights[lightIndex];
        if (light == nullptr) {
            continue;
        }
        SceneLightAsset sceneLight;
        sceneLight.nodeIndex = -1;
        const std::string name = assimpString(light->mName);
        for (size_t nodeIndex = 0; nodeIndex < out.scene.nodes.size(); ++nodeIndex) {
            if (out.scene.nodes[nodeIndex].name == name) {
                sceneLight.nodeIndex = static_cast<int32_t>(nodeIndex);
                sceneLight.transform = nodeWorldTransform(nodeIndex);
                break;
            }
        }
        sceneLight.color = glm::vec3{light->mColorDiffuse.r, light->mColorDiffuse.g, light->mColorDiffuse.b};
        const float colorScale = std::max({sceneLight.color.x, sceneLight.color.y, sceneLight.color.z, 1.0e-4f});
        sceneLight.intensity = colorScale;
        sceneLight.color /= colorScale;
        sceneLight.type = light->mType == aiLightSource_DIRECTIONAL ? 0u : light->mType == aiLightSource_AREA ? 2u : light->mType == aiLightSource_SPOT ? 3u : 1u;
        sceneLight.innerConeRadians = light->mAngleInnerCone;
        sceneLight.outerConeRadians = light->mAngleOuterCone;
        const float attenuationLinear = light->mAttenuationLinear;
        const float attenuationQuadratic = light->mAttenuationQuadratic;
        if (attenuationQuadratic > 1.0e-6f) {
            sceneLight.sizeOrRadius = std::sqrt(1.0f / attenuationQuadratic);
        } else if (attenuationLinear > 1.0e-6f) {
            sceneLight.sizeOrRadius = 1.0f / attenuationLinear;
        }
        out.scene.lights.push_back(sceneLight);
    }

    if (out.scene.meshes.empty()) {
        out.errors.push_back("FBX source did not contain renderable static meshes.");
    }

    // FBX scene unit/axis metadata. Assimp's FBX importer already bakes the
    // source up-axis/handedness conversion into the root node transform during
    // ReadFile, so geometry already arrives in the engine's Y-up space and the
    // existing placement paths stay correct. Unit scale is reported but not
    // auto-applied: the FBX UnitScaleFactor is centimeter-relative (1.0 == 1 cm)
    // and the existing runtime treats imported FBX units as scene units, so
    // silently rescaling here would regress every already-working FBX placement.
    // Surfacing the metadata makes the unit/axis provenance debuggable and lets
    // downstream tooling decide on an explicit rescale.
    float fbxUnitScaleFactor = 1.0f;
    int fbxUpAxis = 1;
    int fbxUpAxisSign = 1;
    int fbxFrontAxis = 2;
    int fbxFrontAxisSign = 1;
    int fbxCoordAxis = 0;
    int fbxCoordAxisSign = 1;
    int fbxOriginalUpAxis = 1;
    bool fbxUnitScaleFactorAuthored = false;
    bool fbxAxisMetadataAuthored = false;
    if (imported->mMetaData != nullptr) {
        if (imported->mMetaData->Get("UnitScaleFactor", fbxUnitScaleFactor)) {
            fbxUnitScaleFactorAuthored = true;
        } else {
            double unitScaleDouble = 1.0;
            if (imported->mMetaData->Get("UnitScaleFactor", unitScaleDouble)) {
                fbxUnitScaleFactor = static_cast<float>(unitScaleDouble);
                fbxUnitScaleFactorAuthored = true;
            }
        }
        fbxAxisMetadataAuthored = imported->mMetaData->Get("UpAxis", fbxUpAxis);
        imported->mMetaData->Get("UpAxisSign", fbxUpAxisSign);
        imported->mMetaData->Get("FrontAxis", fbxFrontAxis);
        imported->mMetaData->Get("FrontAxisSign", fbxFrontAxisSign);
        imported->mMetaData->Get("CoordAxis", fbxCoordAxis);
        imported->mMetaData->Get("CoordAxisSign", fbxCoordAxisSign);
        if (!imported->mMetaData->Get("OriginalUpAxis", fbxOriginalUpAxis)) {
            fbxOriginalUpAxis = fbxUpAxis;
        }
    }
    const bool fbxUnitIsNonMeter = fbxUnitScaleFactorAuthored && std::abs(fbxUnitScaleFactor - 100.0f) > 1.0e-3f;
    const float fbxMetersPerUnit = fbxUnitScaleFactor / 100.0f;
    if (fbxUnitScaleFactorAuthored && std::abs(fbxUnitScaleFactor - 1.0f) > 1.0e-3f && std::abs(fbxUnitScaleFactor - 100.0f) > 1.0e-3f) {
        out.warnings.push_back("FBX UnitScaleFactor is " + std::to_string(fbxUnitScaleFactor) +
            " cm/unit; the engine imports FBX geometry at source scale. Apply an explicit scale of " +
            std::to_string(fbxMetersPerUnit) + " to convert this FBX to engine meters.");
    }

    out.diagnostics = {
        {"schema", "FbxStaticImportDiagnosticsV1"},
        {"parser", "assimp"},
        {"supported", true},
        {"sceneMetrics", {
            {"unitScaleFactor", fbxUnitScaleFactor},
            {"unitScaleFactorAuthored", fbxUnitScaleFactorAuthored},
            {"metersPerUnit", fbxMetersPerUnit},
            {"unitIsNonMeter", fbxUnitIsNonMeter},
            {"unitConversionPolicy", "reported_not_auto_applied_source_scale_preserved"},
            {"upAxis", fbxUpAxis},
            {"upAxisSign", fbxUpAxisSign},
            {"frontAxis", fbxFrontAxis},
            {"frontAxisSign", fbxFrontAxisSign},
            {"coordAxis", fbxCoordAxis},
            {"coordAxisSign", fbxCoordAxisSign},
            {"originalUpAxis", fbxOriginalUpAxis},
            {"axisMetadataAuthored", fbxAxisMetadataAuthored},
            {"axisConversionPolicy", "assimp_bakes_source_axis_to_root_transform"},
            {"axisConversionImplemented", true},
            {"unitMetadataImportImplemented", true},
        }},
        {"meshCount", out.scene.meshes.size()},
        {"materialCount", out.scene.materials.size()},
        {"textureCount", out.assets.textures().size()},
        {"nodeCount", out.scene.nodes.size()},
        {"hierarchy", {
            {"preserved", true},
            {"rootNodes", out.scene.rootNodes},
            {"nodeReports", nodeReports},
            {"metadataPropertyCount", nodeMetadataPropertyCount},
            {"hiddenNodeCount", hiddenNodeCount},
            {"nonCameraVisibleNodeCount", nonCameraVisibleNodeCount},
            {"nonShadowCasterNodeCount", nonShadowCasterNodeCount},
            {"nonShadowReceiverNodeCount", nonShadowReceiverNodeCount},
            {"renderLayerOverrideNodeCount", renderLayerOverrideNodeCount},
            {"negativeScaleNodeCount", negativeScaleNodeCount},
            {"nonUniformScaleNodeCount", nonUniformScaleNodeCount},
            {"fbxAuthoredGeometricOpNodeCount", fbxAuthoredGeometricOpNodeCount},
            {"visibilityMetadataMapped", hiddenNodeCount > 0 || nonCameraVisibleNodeCount > 0},
            {"shadowMetadataMapped", nonShadowCasterNodeCount > 0 || nonShadowReceiverNodeCount > 0},
            {"renderLayerMetadataMapped", renderLayerOverrideNodeCount > 0},
            {"geometricTransformDiagnosticsImplemented", true},
            {"pivotPrePostRotationDiagnosticsImplemented", true},
            {"negativeScaleDiagnosticsImplemented", true},
            {"objectNamesPreserved", true},
            {"sourceNodeIndicesPreserved", true},
        }},
        {"cameraCount", imported->mNumCameras},
        {"lightCount", imported->mNumLights},
        {"animationCount", imported->mNumAnimations},
        {"skeletonCount", out.skeletons.size()},
        {"textureConvention", {
            {"baseColorAlphaIsOpacity", textureConvention.baseColorAlphaIsOpacity},
            {"specularIsPackedOcclusionRoughnessMetalness", textureConvention.specularIsPackedOcclusionRoughnessMetalness},
            {"normalMapIsDirectX", textureConvention.normalMapIsDirectX},
            {"source", textureConvention.source},
        }},
        {"pyscene", {
            {"found", pysceneOverrides.found},
            {"path", pysceneOverrides.path.empty() ? std::string{} : pysceneOverrides.path.generic_string()},
            {"environmentMap", pysceneOverrides.environmentMap.has_value() ? pysceneOverrides.environmentMap->generic_string() : std::string{}},
            {"environmentIntensity", pysceneOverrides.environmentIntensity.value_or(0.0f)},
            {"globalEmissiveMultiplier", pysceneOverrides.globalEmissiveMultiplier},
            {"materialOverrideCount", pysceneOverrides.materials.size()},
            {"applied", pysceneOverrides.applied},
            {"ignored", pysceneOverrides.ignored},
        }},
        {"emissiveScaleImportSetting", {
            {"requestedScale", importEmissiveScale},
            {"requested", importEmissiveScaleRequested},
            {"applied", importEmissiveScaleApplies},
            {"precedence", pysceneOverrides.found ? "pyscene_sidecar" : "import_setting"},
        }},
        {"skeletons", out.skeletons},
        {"animations", out.animations},
        {"textures", textureReports},
        {"meshes", meshReports},
        {"materials", materialReports},
    };
#else
    (void)sourcePath;
    (void)displayName;
    (void)textureFormatSupport;
    out.supported = false;
    out.errors.push_back("FBX static import requires RTV_ENABLE_ASSIMP_IMPORTER=ON and Assimp availability.");
    out.diagnostics = {
        {"schema", "FbxStaticImportDiagnosticsV1"},
        {"parser", "assimp"},
        {"supported", false},
        {"disabledReason", "RTV_ENABLE_ASSIMP_IMPORTER=OFF or Assimp unavailable"},
    };
#endif
    return out;
}

struct UsdStageImportData {
    bool supported = false;
    nlohmann::json diagnostics = nlohmann::json::object();
    nlohmann::json prims = nlohmann::json::array();
    nlohmann::json rootPrims = nlohmann::json::array();
    nlohmann::json animations = nlohmann::json::array();
    nlohmann::json cameras = nlohmann::json::array();
    nlohmann::json lights = nlohmann::json::array();
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    size_t meshCount = 0;
    size_t materialBindingCount = 0;
    size_t cameraCount = 0;
    size_t lightCount = 0;
    double metersPerUnit = 0.0;
    std::string upAxis;
    bool hasTimeSamples = false;
    double startTimeCode = 0.0;
    double endTimeCode = 0.0;
    double timeCodesPerSecond = 24.0;
    size_t timeSampledTransformPrimCount = 0;
    size_t timeSampledPointsPrimCount = 0;
    size_t timeSampledVisibilityPrimCount = 0;
    size_t decodedTransformAnimationChannelCount = 0;
    size_t decodedTransformAnimationKeyframeCount = 0;
    size_t decodedMeshPointAnimationChannelCount = 0;
    size_t decodedMeshPointAnimationKeyframeCount = 0;
    size_t decodedCameraLightAnimationChannelCount = 0;
    size_t decodedCameraLightAnimationKeyframeCount = 0;
    size_t referencePrimCount = 0;
    size_t payloadPrimCount = 0;
    size_t variantSetPrimCount = 0;
    size_t variantSetCount = 0;
    size_t instancePrimCount = 0;
    size_t sublayerCount = 0;
    size_t invisiblePrimCount = 0;
    size_t inactivePrimCount = 0;
    size_t guidePurposePrimCount = 0;
    size_t proxyPurposePrimCount = 0;
    size_t renderPurposePrimCount = 0;
    size_t prototypeCount = 0;
    size_t instancedPrototypeMeshPrimCount = 0;
};

struct UsdRuntimeMeshCookData {
    bool supported = false;
    std::vector<MeshAsset> meshes;
    std::vector<std::string> materialBindingPaths;
    std::vector<std::vector<std::string>> primitiveMaterialBindingPaths;
    nlohmann::json diagnostics = nlohmann::json::object();
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    size_t meshPrimCount = 0;
    size_t skippedMeshPrimCount = 0;
    size_t materialBindingTargetCount = 0;
};

struct UsdSceneEntityImportData {
    bool supported = false;
    nlohmann::json diagnostics = nlohmann::json::object();
    nlohmann::json cameras = nlohmann::json::array();
    nlohmann::json lights = nlohmann::json::array();
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    size_t cameraCount = 0;
    size_t lightCount = 0;
};

struct UsdMaterialShaderNetworkData {
    MaterialAsset material;
    nlohmann::json diagnostics = nlohmann::json::object();
    bool shaderNetworkConverted = false;
};

struct UsdMaterialShaderNetworkRequest {
    std::string materialPath;
    std::string materialName;
};

struct UsdzPackageEntry {
    std::string path;
    uint16_t compressionMethod = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint32_t localHeaderOffset = 0;
    bool isTexture = false;
    bool isStage = false;
};

struct UsdzPackageTextureData {
    bool inspected = false;
    bool isUsdz = false;
    size_t textureEntryCount = 0;
    size_t stageEntryCount = 0;
    nlohmann::json diagnostics = nlohmann::json::object();
    std::vector<UsdzPackageEntry> entries;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

uint16_t readLe16(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 2u > bytes.size()) {
        return 0u;
    }
    return static_cast<uint16_t>(bytes[offset]) | (static_cast<uint16_t>(bytes[offset + 1u]) << 8u);
}

uint32_t readLe32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 4u > bytes.size()) {
        return 0u;
    }
    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

bool usdPackagePathIsTexture(std::string_view path) {
    const std::string ext = lowerString(std::filesystem::path(path).extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" ||
        ext == ".hdr" || ext == ".exr" || ext == ".ktx" || ext == ".ktx2" ||
        ext == ".dds" || ext == ".basis";
}

bool usdPackagePathIsStage(std::string_view path) {
    const std::string ext = lowerString(std::filesystem::path(path).extension().string());
    return ext == ".usd" || ext == ".usda" || ext == ".usdc";
}

UsdzPackageTextureData inspectUsdzPackageTextures(const std::filesystem::path& sourcePath) {
    UsdzPackageTextureData out;
    out.isUsdz = lowerString(sourcePath.extension().string()) == ".usdz";
    nlohmann::json entries = nlohmann::json::array();
    if (!out.isUsdz) {
        out.diagnostics = {
            {"schema", "UsdzPackageTextureDiagnosticsV1"},
            {"sourcePath", sourcePath.generic_string()},
            {"isUsdz", false},
            {"inspected", false},
            {"textureProvenanceInspectionImplemented", false},
            {"textureExtractionImplemented", false},
            {"textureNativeCookImplemented", false},
            {"textureEntryCount", 0},
            {"stageEntryCount", 0},
            {"entries", entries},
        };
        return out;
    }

    std::ifstream file(sourcePath, std::ios::binary);
    if (!file) {
        out.errors.push_back("Could not open USDZ package for texture provenance inspection.");
    } else {
        file.seekg(0, std::ios::end);
        const std::streamoff fileSize = file.tellg();
        if (fileSize <= 0) {
            out.errors.push_back("USDZ package is empty.");
        } else {
            const size_t tailSize = static_cast<size_t>(std::min<std::streamoff>(fileSize, 66000));
            std::vector<uint8_t> tail(tailSize);
            file.seekg(fileSize - static_cast<std::streamoff>(tailSize), std::ios::beg);
            file.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
            if (file.gcount() != static_cast<std::streamsize>(tail.size())) {
                out.errors.push_back("Could not read USDZ package directory tail.");
            } else {
                size_t eocd = std::string::npos;
                for (size_t i = tail.size(); i >= 4u; --i) {
                    const size_t pos = i - 4u;
                    if (readLe32(tail, pos) == 0x06054b50u) {
                        eocd = pos;
                        break;
                    }
                }
                if (eocd == std::string::npos || eocd + 22u > tail.size()) {
                    out.errors.push_back("USDZ package central directory was not found.");
                } else {
                    const uint16_t entryCount = readLe16(tail, eocd + 10u);
                    const uint32_t centralDirectorySize = readLe32(tail, eocd + 12u);
                    const uint32_t centralDirectoryOffset = readLe32(tail, eocd + 16u);
                    if (static_cast<uint64_t>(centralDirectoryOffset) + centralDirectorySize > static_cast<uint64_t>(fileSize)) {
                        out.errors.push_back("USDZ package central directory range is invalid.");
                    } else {
                        std::vector<uint8_t> directory(centralDirectorySize);
                        file.seekg(static_cast<std::streamoff>(centralDirectoryOffset), std::ios::beg);
                        file.read(reinterpret_cast<char*>(directory.data()), static_cast<std::streamsize>(directory.size()));
                        if (file.gcount() != static_cast<std::streamsize>(directory.size())) {
                            out.errors.push_back("Could not read USDZ package central directory.");
                        } else {
                            size_t offset = 0;
                            for (uint16_t i = 0; i < entryCount && offset + 46u <= directory.size(); ++i) {
                                if (readLe32(directory, offset) != 0x02014b50u) {
                                    out.warnings.push_back("Stopped USDZ package inspection at an unexpected central directory record.");
                                    break;
                                }
                                const uint16_t compression = readLe16(directory, offset + 10u);
                                const uint32_t compressedSize = readLe32(directory, offset + 20u);
                                const uint32_t uncompressedSize = readLe32(directory, offset + 24u);
                                const uint16_t nameLength = readLe16(directory, offset + 28u);
                                const uint16_t extraLength = readLe16(directory, offset + 30u);
                                const uint16_t commentLength = readLe16(directory, offset + 32u);
                                const uint32_t localHeaderOffset = readLe32(directory, offset + 42u);
                                const size_t nameOffset = offset + 46u;
                                const size_t nextOffset = nameOffset + nameLength + extraLength + commentLength;
                                if (nextOffset > directory.size()) {
                                    out.warnings.push_back("Stopped USDZ package inspection at a truncated central directory record.");
                                    break;
                                }
                                const std::string entryName(reinterpret_cast<const char*>(directory.data() + nameOffset), nameLength);
                                const bool isTexture = usdPackagePathIsTexture(entryName);
                                const bool isStage = usdPackagePathIsStage(entryName);
                                if (isTexture || isStage) {
                                    out.entries.push_back(UsdzPackageEntry{
                                        .path = entryName,
                                        .compressionMethod = compression,
                                        .compressedSize = compressedSize,
                                        .uncompressedSize = uncompressedSize,
                                        .localHeaderOffset = localHeaderOffset,
                                        .isTexture = isTexture,
                                        .isStage = isStage,
                                    });
                                    entries.push_back({
                                        {"path", entryName},
                                        {"extension", lowerString(std::filesystem::path(entryName).extension().string())},
                                        {"kind", isTexture ? "texture" : "stage"},
                                        {"compressionMethod", compression},
                                        {"stored", compression == 0u},
                                        {"localHeaderOffset", localHeaderOffset},
                                        {"compressedSize", compressedSize},
                                        {"uncompressedSize", uncompressedSize},
                                        {"textureExtractionImplemented", false},
                                        {"nativeTextureCookImplemented", false},
                                    });
                                    out.textureEntryCount += isTexture ? 1u : 0u;
                                    out.stageEntryCount += isStage ? 1u : 0u;
                                }
                                offset = nextOffset;
                            }
                            out.inspected = true;
                        }
                    }
                }
            }
        }
    }
    if (out.inspected && out.textureEntryCount > 0) {
        out.warnings.push_back("USDZ packaged texture provenance was preserved; extraction, native .rttexture cook, and shader binding are handled by the later USD package texture and material cook stages when texture import is enabled.");
    }
    out.diagnostics = {
        {"schema", "UsdzPackageTextureDiagnosticsV1"},
        {"sourcePath", sourcePath.generic_string()},
        {"isUsdz", true},
        {"inspected", out.inspected},
        {"textureProvenanceInspectionImplemented", out.inspected},
        {"textureExtractionImplemented", false},
        {"textureNativeCookImplemented", false},
        {"textureEntryCount", out.textureEntryCount},
        {"stageEntryCount", out.stageEntryCount},
        {"entries", entries},
        {"warnings", out.warnings},
        {"errors", out.errors},
    };
    return out;
}

std::optional<std::vector<uint8_t>> extractUsdzEntryBytes(
    const std::filesystem::path& sourcePath,
    const UsdzPackageEntry& entry,
    std::string& error) {
    std::ifstream file(sourcePath, std::ios::binary);
    if (!file) {
        error = "could_not_open_usdz";
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize <= 0 || static_cast<uint64_t>(entry.localHeaderOffset) + 30u > static_cast<uint64_t>(fileSize)) {
        error = "invalid_local_header_offset";
        return std::nullopt;
    }
    std::array<uint8_t, 30> localHeader{};
    file.seekg(static_cast<std::streamoff>(entry.localHeaderOffset), std::ios::beg);
    file.read(reinterpret_cast<char*>(localHeader.data()), static_cast<std::streamsize>(localHeader.size()));
    if (file.gcount() != static_cast<std::streamsize>(localHeader.size()) || readLe32(std::vector<uint8_t>(localHeader.begin(), localHeader.end()), 0) != 0x04034b50u) {
        error = "invalid_local_header";
        return std::nullopt;
    }
    const std::vector<uint8_t> headerBytes(localHeader.begin(), localHeader.end());
    const uint16_t nameLength = readLe16(headerBytes, 26u);
    const uint16_t extraLength = readLe16(headerBytes, 28u);
    const uint64_t dataOffset = static_cast<uint64_t>(entry.localHeaderOffset) + 30u + nameLength + extraLength;
    if (dataOffset + entry.compressedSize > static_cast<uint64_t>(fileSize)) {
        error = "entry_payload_out_of_range";
        return std::nullopt;
    }
    std::vector<uint8_t> compressed(entry.compressedSize);
    file.seekg(static_cast<std::streamoff>(dataOffset), std::ios::beg);
    file.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
    if (file.gcount() != static_cast<std::streamsize>(compressed.size())) {
        error = "could_not_read_entry_payload";
        return std::nullopt;
    }
    if (entry.compressionMethod == 0u) {
        if (entry.uncompressedSize != 0u && entry.uncompressedSize != compressed.size()) {
            error = "stored_size_mismatch";
            return std::nullopt;
        }
        return compressed;
    }
    if (entry.compressionMethod == 8u) {
        if (entry.uncompressedSize == 0u) {
            error = "deflated_entry_missing_uncompressed_size";
            return std::nullopt;
        }
        std::vector<uint8_t> inflated(entry.uncompressedSize);
        const int decoded = stbi_zlib_decode_noheader_buffer(
            reinterpret_cast<char*>(inflated.data()),
            static_cast<int>(inflated.size()),
            reinterpret_cast<const char*>(compressed.data()),
            static_cast<int>(compressed.size()));
        if (decoded != static_cast<int>(inflated.size())) {
            error = "deflate_decode_failed";
            return std::nullopt;
        }
        return inflated;
    }
    error = "unsupported_zip_compression_method_" + std::to_string(entry.compressionMethod);
    return std::nullopt;
}

TextureAsset textureAssetFromData(
    const TextureData& textureData,
    std::string name,
    std::filesystem::path sourcePath,
    NativeTextureColorSpace colorSpace) {
    TextureAsset texture;
    texture.name = std::move(name);
    texture.sourcePath = std::move(sourcePath);
    texture.width = static_cast<uint32_t>(std::max(1, textureData.width));
    texture.height = static_cast<uint32_t>(std::max(1, textureData.height));
    texture.channels = 4;
    texture.sourceArrayLayers = textureData.sourceArrayLayers;
    texture.sourceDepth = textureData.sourceDepth;
    texture.sourceFaceCount = textureData.sourceFaceCount;
    texture.sourceIsCubemap = textureData.sourceIsCubemap;
    texture.mipLevels = std::max(1, textureData.mipLevels);
    texture.srgb = colorSpace == NativeTextureColorSpace::Srgb && !textureData.linearColorSpace;
    texture.linearColorSpace = colorSpace != NativeTextureColorSpace::Srgb || textureData.linearColorSpace;
    texture.format = textureData.format;
    texture.isCompressed = textureData.isCompressed;
    texture.compressedFormat = textureData.compressedFormat;
    texture.sourceContainerKind = textureData.sourceContainerKind;
    texture.nativePayloadSource = textureData.nativePayloadSource;
    texture.sourceContainerPreserved = textureData.sourceContainerPreserved;
    texture.sourceContainerTranscoded = textureData.sourceContainerTranscoded;
    texture.rgba8 = textureData.pixels;
    texture.mipData = textureData.mipData;
    return texture;
}

NativeTextureRole usdTextureRoleForAttribute(std::string_view attributeName) {
    const std::string lower = lowerString(std::string(attributeName));
    if (lower.find("clearcoatroughness") != std::string::npos || lower.find("clearcoat_roughness") != std::string::npos) {
        return NativeTextureRole::ClearcoatRoughness;
    }
    if (lower.find("clearcoatnormal") != std::string::npos || lower.find("clearcoat_normal") != std::string::npos) {
        return NativeTextureRole::ClearcoatNormal;
    }
    if (lower.find("clearcoat") != std::string::npos) {
        return NativeTextureRole::Clearcoat;
    }
    if (lower.find("normal") != std::string::npos) {
        return NativeTextureRole::Normal;
    }
    if (lower.find("transmission") != std::string::npos) {
        return NativeTextureRole::Transmission;
    }
    if (lower.find("thickness") != std::string::npos || lower.find("volume") != std::string::npos) {
        return NativeTextureRole::Thickness;
    }
    if (lower.find("specularcolor") != std::string::npos || lower.find("specular_color") != std::string::npos) {
        return NativeTextureRole::SpecularColor;
    }
    if (lower.find("specular") != std::string::npos) {
        return NativeTextureRole::Specular;
    }
    if (lower.find("metallic") != std::string::npos || lower.find("roughness") != std::string::npos) {
        return NativeTextureRole::MetallicRoughness;
    }
    if (lower.find("occlusion") != std::string::npos || lower.find("ao") != std::string::npos) {
        return NativeTextureRole::Occlusion;
    }
    if (lower.find("emissive") != std::string::npos) {
        return NativeTextureRole::Emissive;
    }
    if (lower.find("opacity") != std::string::npos ||
        lower.find("alpha") != std::string::npos ||
        lower.find("transparency") != std::string::npos ||
        lower.find("mask") != std::string::npos) {
        return NativeTextureRole::Opacity;
    }
    if (lower.find("height") != std::string::npos || lower.find("displacement") != std::string::npos) {
        return NativeTextureRole::Height;
    }
    if (lower.find("basecolor") != std::string::npos ||
        lower.find("base_color") != std::string::npos ||
        lower.find("albedo") != std::string::npos ||
        lower.find("diffuse") != std::string::npos) {
        return NativeTextureRole::BaseColor;
    }
    return NativeTextureRole::Unknown;
}

NativeTextureColorSpace colorSpaceForTextureRole(NativeTextureRole role) {
    return role == NativeTextureRole::BaseColor ||
                   role == NativeTextureRole::Emissive ||
                   role == NativeTextureRole::SpecularColor ||
                   role == NativeTextureRole::SheenColor
        ? NativeTextureColorSpace::Srgb
        : NativeTextureColorSpace::Linear;
}

std::string canonicalUsdPackageEntryPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }
    return lowerString(path);
}

std::vector<std::string> usdTextureLookupKeys(std::string path) {
    std::vector<std::string> keys;
    std::string canonical = canonicalUsdPackageEntryPath(std::move(path));
    if (!canonical.empty()) {
        keys.push_back(canonical);
        const std::filesystem::path filename = std::filesystem::path(canonical).filename();
        const std::string filenameKey = lowerString(filename.generic_string());
        if (!filenameKey.empty() && filenameKey != canonical) {
            keys.push_back(filenameKey);
        }
    }
    return keys;
}

std::optional<std::filesystem::path> resolveUsdExternalTexturePath(
    const std::filesystem::path& stagePath,
    const std::string& authoredPath,
    const std::string& resolvedPath) {
    for (const std::string& candidateString : {resolvedPath, authoredPath}) {
        if (candidateString.empty() || candidateString.find("://") != std::string::npos || isDataUri(candidateString)) {
            continue;
        }
        std::filesystem::path candidate(candidateString);
        if (!candidate.is_absolute()) {
            candidate = stagePath.parent_path() / candidate;
        }
        candidate = candidate.lexically_normal();
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

NativeTextureRole usdTextureRoleForReference(const nlohmann::json& reference) {
    NativeTextureRole role = usdTextureRoleForAttribute(reference.value("attribute", std::string{}));
    if (role != NativeTextureRole::Unknown) {
        return role;
    }
    const std::string shaderPath = reference.value("shaderPath", std::string{});
    role = usdTextureRoleForAttribute(shaderPath);
    if (role != NativeTextureRole::Unknown) {
        return role;
    }
    role = usdTextureRoleForAttribute(reference.value("assetPath", std::string{}));
    if (role != NativeTextureRole::Unknown) {
        return role;
    }
    role = usdTextureRoleForAttribute(reference.value("resolvedPath", std::string{}));
    if (role != NativeTextureRole::Unknown) {
        return role;
    }
    return NativeTextureRole::Unknown;
}

std::string usdMaterialTextureSlotName(NativeTextureRole role) {
    switch (role) {
    case NativeTextureRole::BaseColor: return "baseColor";
    case NativeTextureRole::Normal: return "normal";
    case NativeTextureRole::MetallicRoughness: return "metallicRoughness";
    case NativeTextureRole::Metallic: return "metallicRoughness";
    case NativeTextureRole::Roughness: return "metallicRoughness";
    case NativeTextureRole::Occlusion: return "occlusion";
    case NativeTextureRole::Emissive: return "emissive";
    case NativeTextureRole::Opacity: return "opacity";
    case NativeTextureRole::Height: return "normal";
    case NativeTextureRole::Clearcoat: return "clearcoat";
    case NativeTextureRole::ClearcoatRoughness: return "clearcoatRoughness";
    case NativeTextureRole::ClearcoatNormal: return "clearcoatNormal";
    case NativeTextureRole::Transmission: return "transmission";
    case NativeTextureRole::Thickness: return "volumeThickness";
    case NativeTextureRole::Specular: return "specular";
    case NativeTextureRole::SpecularColor: return "specularColor";
    default: return "unknown";
    }
}

bool bindUsdMaterialTextureSlot(MaterialAsset& material, NativeTextureRole role, uint32_t textureSlot) {
    const TextureAssetHandle handle{textureSlot};
    switch (role) {
    case NativeTextureRole::BaseColor:
        material.baseColorTexture = handle;
        return true;
    case NativeTextureRole::Normal:
        material.normalTexture = handle;
        return true;
    case NativeTextureRole::MetallicRoughness:
    case NativeTextureRole::Metallic:
    case NativeTextureRole::Roughness:
        material.metallicRoughnessTexture = handle;
        return true;
    case NativeTextureRole::Occlusion:
        material.occlusionTexture = handle;
        return true;
    case NativeTextureRole::Emissive:
        material.emissiveTexture = handle;
        return true;
    case NativeTextureRole::Opacity:
        material.opacityTexture = handle;
        return true;
    case NativeTextureRole::Height:
        material.normalTexture = handle;
        return true;
    case NativeTextureRole::Clearcoat:
        material.clearcoatTexture = handle;
        return true;
    case NativeTextureRole::ClearcoatRoughness:
        material.clearcoatRoughnessTexture = handle;
        return true;
    case NativeTextureRole::ClearcoatNormal:
        material.clearcoatNormalTexture = handle;
        return true;
    case NativeTextureRole::Transmission:
        material.transmissionTexture = handle;
        return true;
    case NativeTextureRole::Thickness:
        material.volumeThicknessTexture = handle;
        return true;
    case NativeTextureRole::Specular:
        material.specularTexture = handle;
        return true;
    case NativeTextureRole::SpecularColor:
        material.specularColorTexture = handle;
        return true;
    default:
        return false;
    }
}

std::string usdMaterialNameFromPath(std::string_view path, std::string_view fallbackName) {
    std::string value(path);
    const size_t slash = value.find_last_of('/');
    if (slash != std::string::npos && slash + 1u < value.size()) {
        value = value.substr(slash + 1u);
    }
    if (value.empty()) {
        value = std::string(fallbackName.empty() ? "UsdMaterial" : fallbackName);
    }
    return safeStem(value);
}

UsdMaterialShaderNetworkData defaultUsdMaterialShaderNetworkData(
    const std::filesystem::path& sourcePath,
    std::string_view materialPath,
    std::string_view materialName) {
    UsdMaterialShaderNetworkData out;
    out.material.name = std::string(materialName);
    if (out.material.name.empty()) {
        out.material.name = usdMaterialNameFromPath(materialPath, "UsdMaterial");
    }
    out.diagnostics = {
        {"schema", "UsdMaterialShaderNetworkDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", false},
        {"sourcePath", sourcePath.generic_string()},
        {"sourceMaterialPath", std::string(materialPath)},
        {"shaderNetworkConversionImplemented", false},
        {"textureReferenceExtractionImplemented", false},
        {"textureNativeCookImplemented", false},
        {"textureReferenceCount", 0},
        {"textureReferences", nlohmann::json::array()},
        {"unsupportedShaderNodeCount", 0},
        {"unsupportedShaderNodes", nlohmann::json::array()},
        {"convertedInputCount", 0},
    };
    return out;
}

#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
template <typename T>
bool usdReadAttribute(const pxr::UsdPrim& prim, const char* name, T& out);

nlohmann::json usdMatrixJson(const pxr::GfMatrix4d& matrix) {
    nlohmann::json result = nlohmann::json::array();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            result.push_back(matrix[row][col]);
        }
    }
    return result;
}

glm::mat4 glmMat4FromUsdMatrix(const pxr::GfMatrix4d& matrix) {
    // USD GfMatrix4d uses row-major storage with a row-vector convention
    // (p' = p * M), so translation lives in the last row. glm::mat4 is
    // column-major with a column-vector convention (p' = M * p), so translation
    // lives in the last column. Converting between the two transform conventions
    // is a transpose: glm[col][row] = usd[col][row]. The previous direct logical
    // copy (glm[col][row] = usd[row][col]) silently dropped translation and
    // applied the inverse of any asymmetric rotation.
    glm::mat4 out{1.0f};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            out[col][row] = static_cast<float>(matrix[col][row]);
        }
    }
    return out;
}

glm::vec3 glmVec3FromUsd(const pxr::GfVec3f& value) {
    return glm::vec3{value[0], value[1], value[2]};
}

// Engine runtime convention is Y-up with 1 unit == 1 meter. USD stages may be
// authored Z-up and/or in non-meter units. This builds the matrix that maps a
// stage-space transform into engine space: scale to meters, then rotate Z-up to
// Y-up (rotate -90 deg about X). Returns identity when the stage already matches
// the engine convention so placement output is byte-for-byte unchanged.
glm::mat4 usdStageConversionMatrix(double metersPerUnit, const std::string& upAxis) {
    glm::mat4 conversion{1.0f};
    if (upAxis == "Z") {
        conversion = glm::rotate(glm::mat4{1.0f}, glm::radians(-90.0f), glm::vec3{1.0f, 0.0f, 0.0f});
    }
    const bool metersAuthored = metersPerUnit > 0.0;
    const bool nonMeter = metersAuthored && std::abs(metersPerUnit - 1.0) > 1e-6;
    if (nonMeter) {
        conversion = conversion * glm::scale(glm::mat4{1.0f}, glm::vec3{static_cast<float>(metersPerUnit)});
    }
    return conversion;
}

bool usdStageConversionIsIdentity(const glm::mat4& conversion) {
    return conversion == glm::mat4{1.0f};
}

pxr::UsdPrimRange usdStageImportPrimRange(const pxr::UsdStageRefPtr& stage) {
    pxr::Usd_PrimFlagsPredicate predicate = pxr::UsdPrimDefaultPredicate;
    predicate.TraverseInstanceProxies(true);
    return pxr::UsdPrimRange::Stage(stage, predicate);
}

nlohmann::json usdPrimTransformJson(
    const pxr::UsdPrim& prim,
    pxr::UsdGeomXformCache* xformCache = nullptr,
    const glm::mat4* stageConversion = nullptr,
    bool isRootPrim = false) {
    pxr::UsdGeomXformable xformable(prim);
    if (!xformable) {
        return nlohmann::json::object();
    }
    pxr::GfMatrix4d transform(1.0);
    bool resetsXformStack = false;
    const bool hasTransform = xformable.GetLocalTransformation(&transform, &resetsXformStack);
    const bool applyConversion = stageConversion != nullptr && !usdStageConversionIsIdentity(*stageConversion);
    // The stage-conversion is composed at the top of the hierarchy. Local
    // placement transforms only receive it on root prims (children inherit it
    // through the parented hierarchy), while flat world placements always
    // receive it because they are applied without a converted ancestor.
    glm::mat4 localMatrix = glmMat4FromUsdMatrix(transform);
    if (applyConversion && isRootPrim) {
        localMatrix = (*stageConversion) * localMatrix;
    }
    nlohmann::json result = {
        {"hasTransform", hasTransform},
        {"resetsXformStack", resetsXformStack},
        {"placementTransform", transformJsonFromMatrix(localMatrix)},
        {"localMatrix", usdMatrixJson(transform)},
        {"stageConversionApplied", applyConversion},
    };
    if (xformCache != nullptr) {
        const pxr::GfMatrix4d worldTransform = xformCache->GetLocalToWorldTransform(prim);
        glm::mat4 worldMatrix = glmMat4FromUsdMatrix(worldTransform);
        if (applyConversion) {
            worldMatrix = (*stageConversion) * worldMatrix;
        }
        result["parentHierarchyTransformComposed"] = true;
        result["worldPlacementTransform"] = transformJsonFromMatrix(worldMatrix);
        result["worldMatrix"] = usdMatrixJson(worldTransform);
    } else {
        result["parentHierarchyTransformComposed"] = false;
    }
    return result;
}

nlohmann::json decodedUsdTransformTrackJson(
    const char* targetPath,
    nlohmann::json times,
    nlohmann::json values) {
    return {
        {"decoded", !times.empty() && !values.empty()},
        {"targetPath", targetPath},
        {"interpolation", "LINEAR"},
        {"times", std::move(times)},
        {"values", std::move(values)},
    };
}

void appendUsdDecodedTransformChannel(
    nlohmann::json& channels,
    uint32_t& decodedChannelCount,
    uint32_t& decodedKeyframeCount,
    const std::string& targetNodeName,
    int targetNodeIndex,
    const char* targetPath,
    nlohmann::json decodedTrack,
    uint32_t keyframeCount) {
    if (keyframeCount == 0u) {
        return;
    }
    ++decodedChannelCount;
    decodedKeyframeCount += keyframeCount;
    channels.push_back({
        {"index", channels.size()},
        {"targetNode", targetNodeName},
        {"target", {
            {"node", targetNodeIndex},
            {"nodeName", targetNodeName},
            {"path", targetPath},
        }},
        {"decodedTrack", std::move(decodedTrack)},
    });
}

std::vector<double> usdAttributeTimeSamples(std::initializer_list<pxr::UsdAttribute> attributes) {
    std::vector<double> result;
    for (const pxr::UsdAttribute& attr : attributes) {
        if (!attr) {
            continue;
        }
        std::vector<double> samples;
        attr.GetTimeSamples(&samples);
        result.insert(result.end(), samples.begin(), samples.end());
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void appendUsdDecodedParameterChannel(
    nlohmann::json& channels,
    uint32_t& decodedChannelCount,
    uint32_t& decodedKeyframeCount,
    const std::string& targetNodeName,
    int targetNodeIndex,
    const char* targetPath,
    nlohmann::json times,
    nlohmann::json values) {
    const uint32_t keyframeCount = static_cast<uint32_t>(times.size());
    appendUsdDecodedTransformChannel(
        channels,
        decodedChannelCount,
        decodedKeyframeCount,
        targetNodeName,
        targetNodeIndex,
        targetPath,
        decodedUsdTransformTrackJson(targetPath, std::move(times), std::move(values)),
        keyframeCount);
}

nlohmann::json usdTransformAnimationChannels(
    const pxr::UsdPrim& prim,
    int targetNodeIndex,
    const std::vector<double>& timeSamples,
    double timeCodesPerSecond,
    const glm::mat4* stageConversion,
    bool isRootPrim,
    uint32_t& decodedChannelCount,
    uint32_t& decodedKeyframeCount) {
    nlohmann::json channels = nlohmann::json::array();
    if (targetNodeIndex < 0 || timeSamples.size() <= 1u) {
        return channels;
    }
    pxr::UsdGeomXformable xformable(prim);
    if (!xformable) {
        return channels;
    }

    nlohmann::json times = nlohmann::json::array();
    nlohmann::json translations = nlohmann::json::array();
    nlohmann::json rotations = nlohmann::json::array();
    nlohmann::json scales = nlohmann::json::array();
    for (double timeCode : timeSamples) {
        pxr::GfMatrix4d transform(1.0);
        bool resetsXformStack = false;
        if (!xformable.GetLocalTransformation(&transform, &resetsXformStack, pxr::UsdTimeCode(timeCode))) {
            continue;
        }
        glm::mat4 matrix = glmMat4FromUsdMatrix(transform);
        if (stageConversion != nullptr && isRootPrim && !usdStageConversionIsIdentity(*stageConversion)) {
            matrix = (*stageConversion) * matrix;
        }
        glm::vec3 scale = scaleFromMatrix(matrix);
        glm::mat3 rotationMatrix{matrix};
        if (scale.x > 0.0f) rotationMatrix[0] /= scale.x;
        if (scale.y > 0.0f) rotationMatrix[1] /= scale.y;
        if (scale.z > 0.0f) rotationMatrix[2] /= scale.z;
        times.push_back(timeCodesPerSecond > 0.0 ? timeCode / timeCodesPerSecond : timeCode);
        translations.push_back(vec3Json(glm::vec3(matrix[3])));
        rotations.push_back(quatJson(glm::quat_cast(rotationMatrix)));
        scales.push_back(vec3Json(scale));
    }
    const uint32_t keyframeCount = static_cast<uint32_t>(times.size());
    const std::string targetName = prim.GetName().GetString();
    appendUsdDecodedTransformChannel(
        channels,
        decodedChannelCount,
        decodedKeyframeCount,
        targetName,
        targetNodeIndex,
        "translation",
        decodedUsdTransformTrackJson("translation", times, translations),
        keyframeCount);
    appendUsdDecodedTransformChannel(
        channels,
        decodedChannelCount,
        decodedKeyframeCount,
        targetName,
        targetNodeIndex,
        "rotation",
        decodedUsdTransformTrackJson("rotation", times, rotations),
        keyframeCount);
    appendUsdDecodedTransformChannel(
        channels,
        decodedChannelCount,
        decodedKeyframeCount,
        targetName,
        targetNodeIndex,
        "scale",
        decodedUsdTransformTrackJson("scale", times, scales),
        keyframeCount);
    return channels;
}

nlohmann::json usdCameraParameterAnimationChannels(
    const pxr::UsdGeomCamera& camera,
    int targetNodeIndex,
    double timeCodesPerSecond,
    uint32_t& decodedChannelCount,
    uint32_t& decodedKeyframeCount) {
    nlohmann::json channels = nlohmann::json::array();
    if (targetNodeIndex < 0) {
        return channels;
    }
    const pxr::UsdAttribute focalAttr = camera.GetFocalLengthAttr();
    const pxr::UsdAttribute horizontalAttr = camera.GetHorizontalApertureAttr();
    const pxr::UsdAttribute verticalAttr = camera.GetVerticalApertureAttr();
    const pxr::UsdAttribute clippingAttr = camera.GetClippingRangeAttr();
    const std::vector<double> samples = usdAttributeTimeSamples({focalAttr, horizontalAttr, verticalAttr, clippingAttr});
    if (samples.size() <= 1u) {
        return channels;
    }

    nlohmann::json times = nlohmann::json::array();
    nlohmann::json yfovs = nlohmann::json::array();
    nlohmann::json aspects = nlohmann::json::array();
    nlohmann::json orthoX = nlohmann::json::array();
    nlohmann::json orthoY = nlohmann::json::array();
    nlohmann::json nearFar = nlohmann::json::array();
    for (double timeCode : samples) {
        float horizontalAperture = 20.955f;
        float verticalAperture = 15.2908f;
        float focalLength = 50.0f;
        pxr::GfVec2f clippingRange(0.01f, 1000.0f);
        (void)horizontalAttr.Get(&horizontalAperture, pxr::UsdTimeCode(timeCode));
        (void)verticalAttr.Get(&verticalAperture, pxr::UsdTimeCode(timeCode));
        (void)focalAttr.Get(&focalLength, pxr::UsdTimeCode(timeCode));
        (void)clippingAttr.Get(&clippingRange, pxr::UsdTimeCode(timeCode));
        const float safeFocalLength = std::max(focalLength, 1.0e-4f);
        times.push_back(timeCodesPerSecond > 0.0 ? timeCode / timeCodesPerSecond : timeCode);
        yfovs.push_back(nlohmann::json::array({2.0f * std::atan((verticalAperture * 0.5f) / safeFocalLength)}));
        aspects.push_back(nlohmann::json::array({verticalAperture > 1.0e-6f ? horizontalAperture / verticalAperture : 0.0f}));
        orthoX.push_back(nlohmann::json::array({horizontalAperture}));
        orthoY.push_back(nlohmann::json::array({verticalAperture}));
        nearFar.push_back(nlohmann::json::array({clippingRange[0], clippingRange[1]}));
    }
    const std::string targetName = camera.GetPrim().GetName().GetString();
    if (focalAttr.GetNumTimeSamples() > 1u || verticalAttr.GetNumTimeSamples() > 1u) {
        appendUsdDecodedParameterChannel(channels, decodedChannelCount, decodedKeyframeCount, targetName, targetNodeIndex, "cameraYfov", times, yfovs);
    }
    if (horizontalAttr.GetNumTimeSamples() > 1u || verticalAttr.GetNumTimeSamples() > 1u) {
        appendUsdDecodedParameterChannel(channels, decodedChannelCount, decodedKeyframeCount, targetName, targetNodeIndex, "cameraAspectRatio", times, aspects);
        appendUsdDecodedParameterChannel(channels, decodedChannelCount, decodedKeyframeCount, targetName, targetNodeIndex, "cameraOrthoXmag", times, orthoX);
        appendUsdDecodedParameterChannel(channels, decodedChannelCount, decodedKeyframeCount, targetName, targetNodeIndex, "cameraOrthoYmag", times, orthoY);
    }
    if (clippingAttr.GetNumTimeSamples() > 1u) {
        appendUsdDecodedParameterChannel(channels, decodedChannelCount, decodedKeyframeCount, targetName, targetNodeIndex, "cameraNearFar", times, nearFar);
    }
    return channels;
}

nlohmann::json usdLightParameterAnimationChannels(
    const pxr::UsdPrim& prim,
    int targetNodeIndex,
    double timeCodesPerSecond,
    uint32_t& decodedChannelCount,
    uint32_t& decodedKeyframeCount) {
    nlohmann::json channels = nlohmann::json::array();
    if (targetNodeIndex < 0) {
        return channels;
    }
    const pxr::UsdAttribute intensityAttr = prim.GetAttribute(pxr::TfToken("inputs:intensity"));
    const pxr::UsdAttribute colorAttr = prim.GetAttribute(pxr::TfToken("inputs:color"));
    const pxr::UsdAttribute radiusAttr = prim.GetAttribute(pxr::TfToken("inputs:radius"));
    const pxr::UsdAttribute coneAttr = prim.GetAttribute(pxr::TfToken("inputs:shaping:cone:angle"));
    const std::vector<double> samples = usdAttributeTimeSamples({intensityAttr, colorAttr, radiusAttr, coneAttr});
    if (samples.size() <= 1u) {
        return channels;
    }

    nlohmann::json times = nlohmann::json::array();
    nlohmann::json intensities = nlohmann::json::array();
    nlohmann::json colors = nlohmann::json::array();
    nlohmann::json radii = nlohmann::json::array();
    nlohmann::json coneAngles = nlohmann::json::array();
    for (double timeCode : samples) {
        float intensity = 1.0f;
        float radius = 1.0f;
        float coneAngleDegrees = 40.0f;
        pxr::GfVec3f color(1.0f, 1.0f, 1.0f);
        (void)intensityAttr.Get(&intensity, pxr::UsdTimeCode(timeCode));
        (void)colorAttr.Get(&color, pxr::UsdTimeCode(timeCode));
        (void)radiusAttr.Get(&radius, pxr::UsdTimeCode(timeCode));
        (void)coneAttr.Get(&coneAngleDegrees, pxr::UsdTimeCode(timeCode));
        const float outerConeRadians = std::clamp(coneAngleDegrees, 0.0f, 180.0f) * 0.017453292519943295f;
        times.push_back(timeCodesPerSecond > 0.0 ? timeCode / timeCodesPerSecond : timeCode);
        intensities.push_back(nlohmann::json::array({intensity}));
        colors.push_back(nlohmann::json::array({color[0], color[1], color[2]}));
        radii.push_back(nlohmann::json::array({radius}));
        coneAngles.push_back(nlohmann::json::array({outerConeRadians * 0.5f, outerConeRadians}));
    }
    const std::string targetName = prim.GetName().GetString();
    if (intensityAttr.GetNumTimeSamples() > 1u) {
        appendUsdDecodedParameterChannel(channels, decodedChannelCount, decodedKeyframeCount, targetName, targetNodeIndex, "lightIntensity", times, intensities);
    }
    if (colorAttr.GetNumTimeSamples() > 1u) {
        appendUsdDecodedParameterChannel(channels, decodedChannelCount, decodedKeyframeCount, targetName, targetNodeIndex, "lightColor", times, colors);
    }
    if (radiusAttr.GetNumTimeSamples() > 1u) {
        appendUsdDecodedParameterChannel(channels, decodedChannelCount, decodedKeyframeCount, targetName, targetNodeIndex, "lightRadius", times, radii);
    }
    if (coneAttr.GetNumTimeSamples() > 1u) {
        appendUsdDecodedParameterChannel(channels, decodedChannelCount, decodedKeyframeCount, targetName, targetNodeIndex, "lightConeAngles", times, coneAngles);
    }
    return channels;
}

bool usdPrimHasAuthoredMaterialBinding(const pxr::UsdPrim& prim) {
    return prim.HasRelationship(pxr::TfToken("material:binding"));
}

bool usdReadFloatInputFromPrimRange(const pxr::UsdPrim& root, const char* attributeName, float& value) {
    for (const pxr::UsdPrim& prim : pxr::UsdPrimRange(root)) {
        if (usdReadAttribute(prim, attributeName, value)) {
            return true;
        }
    }
    return false;
}

bool usdReadVec3InputFromPrimRange(const pxr::UsdPrim& root, const char* attributeName, glm::vec3& value) {
    pxr::GfVec3f usdValue(0.0f, 0.0f, 0.0f);
    for (const pxr::UsdPrim& prim : pxr::UsdPrimRange(root)) {
        if (usdReadAttribute(prim, attributeName, usdValue)) {
            value = glmVec3FromUsd(usdValue);
            return true;
        }
    }
    return false;
}

std::string usdTextureReferenceFileKey(
    const std::string& shaderPath,
    const std::string& fileAttribute,
    const std::string& authoredPath,
    const std::string& resolvedPath) {
    return shaderPath + "|" + fileAttribute + "|" + authoredPath + "|" + resolvedPath;
}

void usdAppendMaterialTextureReference(
    nlohmann::json& references,
    std::unordered_set<std::string>& seen,
    std::unordered_set<std::string>& semanticallyBoundFiles,
    const pxr::UsdPrim& texturePrim,
    const pxr::UsdAttribute& fileAttribute,
    const pxr::SdfAssetPath& assetPath,
    const std::string& attributeName,
    const std::string& semanticSourcePath = {},
    const std::string& connectedOutputPath = {}) {
    const std::string authoredPath = assetPath.GetAssetPath();
    const std::string resolvedPath = assetPath.GetResolvedPath();
    if (authoredPath.empty() && resolvedPath.empty()) {
        return;
    }

    const std::string shaderPath = texturePrim.GetPath().GetString();
    const std::string fileAttributeName = fileAttribute.GetName().GetString();
    const std::string fileKey = usdTextureReferenceFileKey(shaderPath, fileAttributeName, authoredPath, resolvedPath);
    const std::string key = shaderPath + "|" + attributeName + "|" + fileAttributeName + "|" + authoredPath + "|" + resolvedPath;
    if (!seen.insert(key).second) {
        return;
    }
    if (!semanticSourcePath.empty()) {
        semanticallyBoundFiles.insert(fileKey);
    }

    pxr::TfToken shaderId;
    (void)usdReadAttribute(texturePrim, "info:id", shaderId);
    pxr::TfToken sourceColorSpace;
    (void)usdReadAttribute(texturePrim, "inputs:sourceColorSpace", sourceColorSpace);

    nlohmann::json reference = {
        {"shaderPath", shaderPath},
        {"shaderTypeName", texturePrim.GetTypeName().GetString()},
        {"shaderId", shaderId.GetString()},
        {"attribute", attributeName},
        {"fileAttribute", fileAttributeName},
        {"assetPath", authoredPath},
        {"resolvedPath", resolvedPath},
        {"nativeTextureCookImplemented", false},
    };
    if (!semanticSourcePath.empty()) {
        reference["semanticSourcePath"] = semanticSourcePath;
        reference["semanticTextureBinding"] = true;
    }
    if (!connectedOutputPath.empty()) {
        reference["connectedOutputPath"] = connectedOutputPath;
    }
    if (!sourceColorSpace.IsEmpty()) {
        reference["sourceColorSpace"] = sourceColorSpace.GetString();
    }
    references.push_back(std::move(reference));
}

bool usdAppendConnectedTextureReferencesFromSource(
    const pxr::UsdPrim& materialRoot,
    const pxr::SdfPath& sourcePath,
    const std::string& semanticAttributeName,
    nlohmann::json& references,
    std::unordered_set<std::string>& seen,
    std::unordered_set<std::string>& semanticallyBoundFiles,
    std::unordered_set<std::string>& visitedSources,
    uint32_t depth = 0u) {
    if (!materialRoot || depth > 8u) {
        return false;
    }
    const std::string sourceKey = sourcePath.GetString();
    if (!visitedSources.insert(sourceKey).second) {
        return false;
    }
    auto stage = materialRoot.GetStage();
    if (!stage) {
        return false;
    }
    const pxr::UsdPrim sourcePrim = stage->GetPrimAtPath(sourcePath.GetPrimPath());
    if (!sourcePrim) {
        return false;
    }

    bool appended = false;
    for (const pxr::UsdAttribute& attribute : sourcePrim.GetAttributes()) {
        pxr::SdfAssetPath assetPath;
        if (attribute.Get(&assetPath, pxr::UsdTimeCode::Default())) {
            usdAppendMaterialTextureReference(
                references,
                seen,
                semanticallyBoundFiles,
                sourcePrim,
                attribute,
                assetPath,
                semanticAttributeName,
                materialRoot.GetPath().GetString(),
                sourceKey);
            appended = true;
        }
    }

    // Follow intermediate utility nodes such as transforms or channel splitters.
    for (const pxr::UsdAttribute& attribute : sourcePrim.GetAttributes()) {
        pxr::SdfPathVector connections;
        if (!attribute.GetConnections(&connections) || connections.empty()) {
            continue;
        }
        for (const pxr::SdfPath& upstream : connections) {
            appended = usdAppendConnectedTextureReferencesFromSource(
                           materialRoot,
                           upstream,
                           semanticAttributeName,
                           references,
                           seen,
                           semanticallyBoundFiles,
                           visitedSources,
                           depth + 1u) ||
                       appended;
        }
    }
    return appended;
}

void usdAppendConnectedTextureReferencesForInput(
    const pxr::UsdPrim& materialRoot,
    const pxr::UsdPrim& shaderPrim,
    const char* semanticAttributeName,
    nlohmann::json& references,
    std::unordered_set<std::string>& seen,
    std::unordered_set<std::string>& semanticallyBoundFiles) {
    const pxr::UsdAttribute semanticAttribute = shaderPrim.GetAttribute(pxr::TfToken(semanticAttributeName));
    if (!semanticAttribute) {
        return;
    }
    pxr::SdfPathVector connections;
    if (!semanticAttribute.GetConnections(&connections) || connections.empty()) {
        return;
    }
    for (const pxr::SdfPath& connection : connections) {
        std::unordered_set<std::string> visitedSources;
        (void)usdAppendConnectedTextureReferencesFromSource(
            materialRoot,
            connection,
            semanticAttributeName,
            references,
            seen,
            semanticallyBoundFiles,
            visitedSources);
    }
}

nlohmann::json usdMaterialTextureReferencesJson(const pxr::UsdPrim& root) {
    nlohmann::json references = nlohmann::json::array();
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> semanticallyBoundFiles;
    static constexpr std::array<const char*, 17> kPreviewSurfaceTextureInputs = {
        "inputs:diffuseColor",
        "inputs:emissiveColor",
        "inputs:normal",
        "inputs:metallic",
        "inputs:roughness",
        "inputs:occlusion",
        "inputs:opacity",
        "inputs:clearcoat",
        "inputs:clearcoatRoughness",
        "inputs:clearcoatNormal",
        "inputs:transmission",
        "inputs:thickness",
        "inputs:specular",
        "inputs:specularColor",
        "inputs:sheenColor",
        "inputs:sheenRoughness",
        "inputs:displacement",
    };
    for (const pxr::UsdPrim& prim : pxr::UsdPrimRange(root)) {
        pxr::TfToken shaderId;
        (void)usdReadAttribute(prim, "info:id", shaderId);
        if (shaderId.GetString() != "UsdPreviewSurface") {
            continue;
        }
        for (const char* inputName : kPreviewSurfaceTextureInputs) {
            usdAppendConnectedTextureReferencesForInput(root, prim, inputName, references, seen, semanticallyBoundFiles);
        }
    }
    for (const pxr::UsdPrim& prim : pxr::UsdPrimRange(root)) {
        for (const pxr::UsdAttribute& attribute : prim.GetAttributes()) {
            pxr::SdfAssetPath assetPath;
            if (!attribute.Get(&assetPath, pxr::UsdTimeCode::Default())) {
                continue;
            }
            const std::string authoredPath = assetPath.GetAssetPath();
            const std::string resolvedPath = assetPath.GetResolvedPath();
            if (authoredPath.empty() && resolvedPath.empty()) {
                continue;
            }
            const std::string attributeName = attribute.GetName().GetString();
            const std::string shaderPath = prim.GetPath().GetString();
            const std::string fileKey = usdTextureReferenceFileKey(shaderPath, attributeName, authoredPath, resolvedPath);
            if (semanticallyBoundFiles.find(fileKey) != semanticallyBoundFiles.end()) {
                continue;
            }
            usdAppendMaterialTextureReference(
                references,
                seen,
                semanticallyBoundFiles,
                prim,
                attribute,
                assetPath,
                attributeName);
        }
    }
    return references;
}

nlohmann::json usdUnsupportedShaderGraphNodesJson(const pxr::UsdPrim& root) {
    nlohmann::json unsupported = nlohmann::json::array();
    static const std::unordered_set<std::string> kSupportedShaderIds = {
        "UsdPreviewSurface",
        "UsdUVTexture",
        "UsdPrimvarReader_float2",
    };
    for (const pxr::UsdPrim& prim : pxr::UsdPrimRange(root)) {
        if (prim.GetTypeName().GetString() != "Shader") {
            continue;
        }
        pxr::TfToken shaderId;
        (void)usdReadAttribute(prim, "info:id", shaderId);
        const std::string shaderIdString = shaderId.GetString();
        if (!shaderIdString.empty() && kSupportedShaderIds.find(shaderIdString) != kSupportedShaderIds.end()) {
            continue;
        }
        unsupported.push_back({
            {"shaderPath", prim.GetPath().GetString()},
            {"shaderId", shaderIdString.empty() ? std::string{"<missing>"} : shaderIdString},
            {"shaderTypeName", prim.GetTypeName().GetString()},
            {"reason", shaderIdString.empty() ? "missing_shader_id" : "unsupported_usd_shader_node"},
            {"runtimeFallback", "ignored_by_usd_preview_surface_factor_converter"},
        });
    }
    return unsupported;
}

std::string usdPathRelativeToMaterialRoot(const pxr::SdfPath& rootPath, const pxr::SdfPath& path) {
    std::string value = path.GetString();
    const std::string root = rootPath.GetString();
    if (!root.empty() && value.rfind(root, 0) == 0) {
        value.erase(0, root.size());
        if (value.empty()) {
            return ".";
        }
        if (value.front() != '/') {
            value.insert(value.begin(), '/');
        }
    }
    return value;
}

nlohmann::json usdAttributeSignatureValue(const pxr::UsdAttribute& attribute) {
    bool boolValue = false;
    if (attribute.Get(&boolValue, pxr::UsdTimeCode::Default())) {
        return boolValue;
    }
    int intValue = 0;
    if (attribute.Get(&intValue, pxr::UsdTimeCode::Default())) {
        return intValue;
    }
    float floatValue = 0.0f;
    if (attribute.Get(&floatValue, pxr::UsdTimeCode::Default())) {
        return floatValue;
    }
    double doubleValue = 0.0;
    if (attribute.Get(&doubleValue, pxr::UsdTimeCode::Default())) {
        return doubleValue;
    }
    pxr::GfVec2f vec2Value;
    if (attribute.Get(&vec2Value, pxr::UsdTimeCode::Default())) {
        return nlohmann::json::array({vec2Value[0], vec2Value[1]});
    }
    pxr::GfVec3f vec3Value;
    if (attribute.Get(&vec3Value, pxr::UsdTimeCode::Default())) {
        return nlohmann::json::array({vec3Value[0], vec3Value[1], vec3Value[2]});
    }
    pxr::GfVec4f vec4Value;
    if (attribute.Get(&vec4Value, pxr::UsdTimeCode::Default())) {
        return nlohmann::json::array({vec4Value[0], vec4Value[1], vec4Value[2], vec4Value[3]});
    }
    pxr::TfToken tokenValue;
    if (attribute.Get(&tokenValue, pxr::UsdTimeCode::Default())) {
        return tokenValue.GetString();
    }
    std::string stringValue;
    if (attribute.Get(&stringValue, pxr::UsdTimeCode::Default())) {
        return stringValue;
    }
    pxr::SdfAssetPath assetPath;
    if (attribute.Get(&assetPath, pxr::UsdTimeCode::Default())) {
        return {
            {"assetPath", canonicalUsdPackageEntryPath(assetPath.GetAssetPath())},
            {"resolvedPath", canonicalUsdPackageEntryPath(assetPath.GetResolvedPath())},
        };
    }
    return nullptr;
}

std::string usdMaterialSourceNetworkKeyOpenUsd(const pxr::UsdPrim& materialPrim) {
    if (!materialPrim) {
        return {};
    }
    const pxr::SdfPath rootPath = materialPrim.GetPath();
    nlohmann::json signature = nlohmann::json::array();
    for (const pxr::UsdPrim& prim : pxr::UsdPrimRange(materialPrim)) {
        if (!prim) {
            continue;
        }
        nlohmann::json primSignature = {
            {"path", usdPathRelativeToMaterialRoot(rootPath, prim.GetPath())},
            {"typeName", prim.GetTypeName().GetString()},
        };
        nlohmann::json attributes = nlohmann::json::array();
        for (const pxr::UsdAttribute& attribute : prim.GetAttributes()) {
            const std::string attributeName = attribute.GetName().GetString();
            nlohmann::json attributeSignature = {
                {"name", attributeName},
                {"type", attribute.GetTypeName().GetAsToken().GetString()},
            };
            nlohmann::json value = usdAttributeSignatureValue(attribute);
            if (!value.is_null()) {
                attributeSignature["value"] = std::move(value);
            }
            pxr::SdfPathVector connections;
            if (attribute.GetConnections(&connections) && !connections.empty()) {
                nlohmann::json connectionJson = nlohmann::json::array();
                for (const pxr::SdfPath& connection : connections) {
                    connectionJson.push_back(usdPathRelativeToMaterialRoot(rootPath, connection));
                }
                attributeSignature["connections"] = std::move(connectionJson);
            }
            attributes.push_back(std::move(attributeSignature));
        }
        primSignature["attributes"] = std::move(attributes);
        signature.push_back(std::move(primSignature));
    }
    return normalizedJsonHash(signature);
}

UsdMaterialShaderNetworkData loadUsdMaterialShaderNetworkOpenUsd(
    const std::filesystem::path& sourcePath,
    pxr::UsdStageRefPtr stage,
    std::string_view materialPath,
    std::string_view materialName) {
    UsdMaterialShaderNetworkData out;
    out.material.name = std::string(materialName.empty() ? usdMaterialNameFromPath(materialPath, "UsdMaterial") : std::string(materialName));
    const pxr::SdfPath sdfMaterialPath{std::string(materialPath)};
    const pxr::UsdPrim materialPrim = stage ? stage->GetPrimAtPath(sdfMaterialPath) : pxr::UsdPrim();
    nlohmann::json convertedInputs = nlohmann::json::array();
    nlohmann::json textureReferences = nlohmann::json::array();
    nlohmann::json unsupportedShaderNodes = nlohmann::json::array();
    if (!materialPrim) {
        out.diagnostics = {
            {"schema", "UsdMaterialShaderNetworkDiagnosticsV1"},
            {"parser", "OpenUSD"},
            {"supported", true},
            {"sourcePath", sourcePath.generic_string()},
            {"sourceMaterialPath", std::string(materialPath)},
            {"materialPrimFound", false},
            {"shaderNetworkConversionImplemented", false},
            {"textureReferenceExtractionImplemented", false},
            {"textureNativeCookImplemented", false},
            {"textureReferenceCount", 0},
            {"textureReferences", textureReferences},
            {"unsupportedShaderNodeCount", 0},
            {"unsupportedShaderNodes", unsupportedShaderNodes},
            {"convertedInputCount", 0},
            {"convertedInputs", convertedInputs},
        };
        return out;
    }

    glm::vec3 vec3Value{};
    float floatValue = 0.0f;
    if (usdReadVec3InputFromPrimRange(materialPrim, "inputs:diffuseColor", vec3Value)) {
        out.material.baseColorFactor.x = std::clamp(vec3Value.x, 0.0f, 1.0f);
        out.material.baseColorFactor.y = std::clamp(vec3Value.y, 0.0f, 1.0f);
        out.material.baseColorFactor.z = std::clamp(vec3Value.z, 0.0f, 1.0f);
        convertedInputs.push_back("inputs:diffuseColor");
    }
    if (usdReadVec3InputFromPrimRange(materialPrim, "inputs:emissiveColor", vec3Value)) {
        out.material.emissiveFactor = glm::max(vec3Value, glm::vec3{0.0f});
        convertedInputs.push_back("inputs:emissiveColor");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:metallic", floatValue)) {
        out.material.metallicFactor = std::clamp(floatValue, 0.0f, 1.0f);
        convertedInputs.push_back("inputs:metallic");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:roughness", floatValue)) {
        out.material.roughnessFactor = std::clamp(floatValue, 0.04f, 1.0f);
        convertedInputs.push_back("inputs:roughness");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:opacity", floatValue)) {
        out.material.baseColorFactor.w = std::clamp(floatValue, 0.0f, 1.0f);
        out.material.alphaMode = out.material.baseColorFactor.w < 0.999f ? kMaterialAlphaModeBlend : kMaterialAlphaModeOpaque;
        convertedInputs.push_back("inputs:opacity");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:ior", floatValue)) {
        out.material.iorFactor = std::max(floatValue, 1.0f);
        out.material.hasIor = 1u;
        convertedInputs.push_back("inputs:ior");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:transmission", floatValue)) {
        out.material.transmissionFactor = std::clamp(floatValue, 0.0f, 1.0f);
        out.material.hasTransmission = out.material.transmissionFactor > 0.0f ? 1u : 0u;
        convertedInputs.push_back("inputs:transmission");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:clearcoat", floatValue)) {
        out.material.clearcoatFactor = std::clamp(floatValue, 0.0f, 1.0f);
        out.material.hasClearcoat = out.material.clearcoatFactor > 0.0f ? 1u : 0u;
        convertedInputs.push_back("inputs:clearcoat");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:clearcoatRoughness", floatValue)) {
        out.material.clearcoatRoughnessFactor = std::clamp(floatValue, 0.0f, 1.0f);
        convertedInputs.push_back("inputs:clearcoatRoughness");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:occlusion", floatValue)) {
        out.material.occlusionStrength = std::clamp(floatValue, 0.0f, 1.0f);
        convertedInputs.push_back("inputs:occlusion");
    }
    if (usdReadVec3InputFromPrimRange(materialPrim, "inputs:specularColor", vec3Value)) {
        out.material.specularColorFactor = glm::clamp(vec3Value, glm::vec3{0.0f}, glm::vec3{1.0f});
        out.material.hasSpecular = 1u;
        convertedInputs.push_back("inputs:specularColor");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:thickness", floatValue)) {
        out.material.volumeThicknessFactor = std::max(floatValue, 0.0f);
        out.material.hasVolume = out.material.volumeThicknessFactor > 0.0f ? 1u : out.material.hasVolume;
        convertedInputs.push_back("inputs:thickness");
    }
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:attenuationDistance", floatValue)) {
        out.material.volumeAttenuationDistance = std::max(floatValue, 0.0f);
        out.material.hasVolume = out.material.volumeAttenuationDistance > 0.0f ? 1u : out.material.hasVolume;
        convertedInputs.push_back("inputs:attenuationDistance");
    }
    if (usdReadVec3InputFromPrimRange(materialPrim, "inputs:attenuationColor", vec3Value)) {
        out.material.volumeAttenuationColor = glm::clamp(vec3Value, glm::vec3{0.0f}, glm::vec3{1.0f});
        out.material.hasVolume = 1u;
        convertedInputs.push_back("inputs:attenuationColor");
    }
    // UsdPreviewSurface opacityThreshold drives alpha-test masking: a nonzero
    // threshold means the surface is alpha-cutout, not alpha-blended.
    if (usdReadFloatInputFromPrimRange(materialPrim, "inputs:opacityThreshold", floatValue)) {
        if (floatValue > 0.0f) {
            out.material.alphaCutoff = std::clamp(floatValue, 0.0f, 1.0f);
            out.material.alphaMode = kMaterialAlphaModeMask;
        }
        convertedInputs.push_back("inputs:opacityThreshold");
    }
    textureReferences = usdMaterialTextureReferencesJson(materialPrim);
    unsupportedShaderNodes = usdUnsupportedShaderGraphNodesJson(materialPrim);
    out.shaderNetworkConverted = !convertedInputs.empty();
    out.diagnostics = {
        {"schema", "UsdMaterialShaderNetworkDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", true},
        {"sourcePath", sourcePath.generic_string()},
        {"sourceMaterialPath", std::string(materialPath)},
        {"materialPrimFound", true},
        {"shaderNetworkConversionImplemented", out.shaderNetworkConverted},
        {"textureReferenceExtractionImplemented", true},
        {"textureNativeCookImplemented", false},
        {"textureReferenceCount", textureReferences.size()},
        {"textureReferences", textureReferences},
        {"unsupportedShaderNodeCount", unsupportedShaderNodes.size()},
        {"unsupportedShaderNodes", unsupportedShaderNodes},
        {"convertedInputCount", convertedInputs.size()},
        {"convertedInputs", convertedInputs},
    };
    return out;
}

std::string usdMaterialBindingTargetPath(const pxr::UsdPrim& prim) {
    pxr::UsdRelationship binding = prim.GetRelationship(pxr::TfToken("material:binding"));
    if (!binding) {
        return {};
    }
    pxr::SdfPathVector targets;
    if (!binding.GetTargets(&targets) || targets.empty()) {
        return {};
    }
    return targets.front().GetString();
}

bool usdPrimLooksLikeLight(const pxr::UsdPrim& prim) {
    const std::string typeName = prim.GetTypeName().GetString();
    return typeName.find("Light") != std::string::npos ||
        typeName == "DistantLight" || typeName == "SphereLight" || typeName == "RectLight" ||
        typeName == "DiskLight" || typeName == "DomeLight" || typeName == "CylinderLight";
}

nlohmann::json usdMeshMetadataJson(const pxr::UsdGeomMesh& mesh) {
    const pxr::UsdPrim prim = mesh.GetPrim();
    // Subdivision scheme, creases/corners, and orientation are surfaced as
    // diagnostics. The engine triangulates the authored polygon mesh; it does not
    // yet evaluate a subdivision surface, so a non-"none" subdivisionScheme is
    // reported (with subdivisionApplied:false) rather than silently treated as a
    // polygon cage. Orientation (rightHanded/leftHanded) is reported so winding
    // provenance is visible.
    pxr::TfToken subdivisionScheme;
    mesh.GetSubdivisionSchemeAttr().Get(&subdivisionScheme, pxr::UsdTimeCode::Default());
    std::string subdivisionSchemeStr = subdivisionScheme.GetString();
    if (subdivisionSchemeStr.empty()) {
        subdivisionSchemeStr = "catmullClark"; // USD schema default when unauthored.
    }
    const bool isSubdivisionSurface = !subdivisionSchemeStr.empty() && subdivisionSchemeStr != "none";
    pxr::TfToken orientation;
    mesh.GetOrientationAttr().Get(&orientation, pxr::UsdTimeCode::Default());
    std::string orientationStr = orientation.GetString();
    if (orientationStr.empty()) {
        orientationStr = "rightHanded"; // USD schema default.
    }
    pxr::VtArray<int> creaseIndices;
    pxr::VtArray<int> cornerIndices;
    mesh.GetCreaseIndicesAttr().Get(&creaseIndices, pxr::UsdTimeCode::Default());
    mesh.GetCornerIndicesAttr().Get(&cornerIndices, pxr::UsdTimeCode::Default());
    const bool hasCreases = !creaseIndices.empty();
    const bool hasCorners = !cornerIndices.empty();
    return {
        {"hasPoints", mesh.GetPointsAttr().HasAuthoredValueOpinion() || mesh.GetPointsAttr().HasValue()},
        {"hasFaceVertexCounts", mesh.GetFaceVertexCountsAttr().HasAuthoredValueOpinion() || mesh.GetFaceVertexCountsAttr().HasValue()},
        {"hasFaceVertexIndices", mesh.GetFaceVertexIndicesAttr().HasAuthoredValueOpinion() || mesh.GetFaceVertexIndicesAttr().HasValue()},
        {"hasNormals", mesh.GetNormalsAttr().HasAuthoredValueOpinion() || mesh.GetNormalsAttr().HasValue()},
        {"hasUvPrimvar", prim.HasAttribute(pxr::TfToken("primvars:st")) || prim.HasAttribute(pxr::TfToken("primvars:UVMap"))},
        {"hasVertexColorPrimvar", prim.HasAttribute(pxr::TfToken("primvars:displayColor")) || prim.HasAttribute(pxr::TfToken("displayColor"))},
        {"materialBound", usdPrimHasAuthoredMaterialBinding(prim)},
        {"subdivisionScheme", subdivisionSchemeStr},
        {"isSubdivisionSurface", isSubdivisionSurface},
        {"subdivisionApplied", false},
        {"orientation", orientationStr},
        {"hasCreases", hasCreases},
        {"hasCorners", hasCorners},
        {"creaseIndexCount", creaseIndices.size()},
        {"cornerIndexCount", cornerIndices.size()},
    };
}

glm::vec3 normalizedOrFallback(glm::vec3 value, glm::vec3 fallback) {
    const float length2 = glm::dot(value, value);
    return length2 > 1.0e-12f ? value / std::sqrt(length2) : fallback;
}

glm::vec4 normalizedTangentOrFallback(glm::vec3 value, float sign) {
    const float length2 = glm::dot(value, value);
    if (length2 <= 1.0e-12f) {
        return glm::vec4{1.0f, 0.0f, 0.0f, sign};
    }
    return glm::vec4{value / std::sqrt(length2), sign};
}

template <typename T>
bool usdReadAttribute(const pxr::UsdPrim& prim, const char* name, T& out) {
    pxr::UsdAttribute attribute = prim.GetAttribute(pxr::TfToken(name));
    return attribute && attribute.Get(&out, pxr::UsdTimeCode::Default());
}

uint32_t usdLightTypeForTypeName(const std::string& typeName) {
    if (typeName == "DistantLight") {
        return 0u;
    }
    if (typeName == "RectLight" || typeName == "DiskLight" || typeName == "SphereLight" ||
        typeName == "DomeLight" || typeName == "CylinderLight") {
        return 1u;
    }
    if (typeName == "SpotLight") {
        return 2u;
    }
    return 1u;
}

nlohmann::json usdCameraRuntimeJson(const pxr::UsdGeomCamera& camera, pxr::UsdGeomXformCache* xformCache = nullptr, const glm::mat4* stageConversion = nullptr) {
    const pxr::UsdPrim prim = camera.GetPrim();
    pxr::TfToken projectionToken;
    float horizontalAperture = 20.955f;
    float verticalAperture = 15.2908f;
    float focalLength = 50.0f;
    pxr::GfVec2f clippingRange(0.01f, 1000.0f);
    camera.GetProjectionAttr().Get(&projectionToken, pxr::UsdTimeCode::Default());
    camera.GetHorizontalApertureAttr().Get(&horizontalAperture, pxr::UsdTimeCode::Default());
    camera.GetVerticalApertureAttr().Get(&verticalAperture, pxr::UsdTimeCode::Default());
    camera.GetFocalLengthAttr().Get(&focalLength, pxr::UsdTimeCode::Default());
    camera.GetClippingRangeAttr().Get(&clippingRange, pxr::UsdTimeCode::Default());
    const bool orthographic = projectionToken.GetString() == "orthographic";
    const float safeFocalLength = std::max(focalLength, 1.0e-4f);
    const float yFov = orthographic ? 0.0f : 2.0f * std::atan((verticalAperture * 0.5f) / safeFocalLength);
    const float aspect = verticalAperture > 1.0e-6f ? horizontalAperture / verticalAperture : 0.0f;
    // Detect time-sampled camera parameters. Parameter tracks are decoded into
    // generated .rtanim clips during USD stage import; default-time values remain
    // the initial camera state used before playback advances.
    auto attrIsTimeSampled = [](const pxr::UsdAttribute& attr) {
        return attr && attr.GetNumTimeSamples() > 1u;
    };
    const bool focalLengthAnimated = attrIsTimeSampled(camera.GetFocalLengthAttr());
    const bool horizontalApertureAnimated = attrIsTimeSampled(camera.GetHorizontalApertureAttr());
    const bool verticalApertureAnimated = attrIsTimeSampled(camera.GetVerticalApertureAttr());
    const bool clippingRangeAnimated = attrIsTimeSampled(camera.GetClippingRangeAttr());
    const pxr::UsdGeomXformable xformable(prim);
    bool transformAnimated = false;
    if (xformable) {
        std::vector<double> xformSamples;
        transformAnimated = xformable.GetTimeSamples(&xformSamples) && xformSamples.size() > 1u;
    }
    const bool parametersAnimated = focalLengthAnimated || horizontalApertureAnimated ||
        verticalApertureAnimated || clippingRangeAnimated;
    return {
        {"primPath", prim.GetPath().GetString()},
        {"name", prim.GetName().GetString()},
        {"transform", usdPrimTransformJson(prim, xformCache, stageConversion, false)},
        {"runtimeCameraConverted", true},
        {"projection", orthographic ? "orthographic" : "perspective"},
        {"cameraProjection", orthographic ? 1u : 0u},
        {"cameraYfov", yFov},
        {"cameraAspectRatio", aspect},
        {"cameraOrthoXmag", horizontalAperture},
        {"cameraOrthoYmag", verticalAperture},
        {"cameraNear", clippingRange[0]},
        {"cameraFar", clippingRange[1]},
        {"animation", {
            {"transformTimeSampled", transformAnimated},
            {"focalLengthTimeSampled", focalLengthAnimated},
            {"horizontalApertureTimeSampled", horizontalApertureAnimated},
            {"verticalApertureTimeSampled", verticalApertureAnimated},
            {"clippingRangeTimeSampled", clippingRangeAnimated},
            {"parametersTimeSampled", parametersAnimated},
            {"hasAnimation", parametersAnimated || transformAnimated},
            {"animationDetectionImplemented", true},
            {"runtimePlaybackImplemented", parametersAnimated || transformAnimated},
        }},
    };
}

nlohmann::json usdLightRuntimeJson(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache* xformCache = nullptr, const glm::mat4* stageConversion = nullptr) {
    const std::string typeName = prim.GetTypeName().GetString();
    float intensity = 1.0f;
    float radius = 1.0f;
    float coneAngleDegrees = 40.0f;
    pxr::GfVec3f color(1.0f, 1.0f, 1.0f);
    (void)usdReadAttribute(prim, "inputs:intensity", intensity);
    (void)usdReadAttribute(prim, "inputs:color", color);
    (void)usdReadAttribute(prim, "inputs:radius", radius);
    (void)usdReadAttribute(prim, "inputs:shaping:cone:angle", coneAngleDegrees);
    const float outerConeRadians = std::clamp(coneAngleDegrees, 0.0f, 180.0f) * 0.017453292519943295f;
    // Detect time-sampled light parameters. Parameter tracks are decoded into
    // generated .rtanim clips during USD stage import; default-time values remain
    // the initial light state used before playback advances.
    auto lightAttrIsTimeSampled = [&prim](const char* name) {
        const pxr::UsdAttribute attr = prim.GetAttribute(pxr::TfToken(name));
        return attr && attr.GetNumTimeSamples() > 1u;
    };
    const bool intensityAnimated = lightAttrIsTimeSampled("inputs:intensity");
    const bool colorAnimated = lightAttrIsTimeSampled("inputs:color");
    const bool radiusAnimated = lightAttrIsTimeSampled("inputs:radius");
    const bool coneAngleAnimated = lightAttrIsTimeSampled("inputs:shaping:cone:angle");
    const pxr::UsdGeomXformable lightXformable(prim);
    bool lightTransformAnimated = false;
    if (lightXformable) {
        std::vector<double> xformSamples;
        lightTransformAnimated = lightXformable.GetTimeSamples(&xformSamples) && xformSamples.size() > 1u;
    }
    const bool lightParametersAnimated = intensityAnimated || colorAnimated ||
        radiusAnimated || coneAngleAnimated;
    return {
        {"primPath", prim.GetPath().GetString()},
        {"name", prim.GetName().GetString()},
        {"typeName", typeName},
        {"transform", usdPrimTransformJson(prim, xformCache, stageConversion, false)},
        {"runtimeLightConverted", true},
        {"lightType", usdLightTypeForTypeName(typeName)},
        {"color", {color[0], color[1], color[2]}},
        {"intensity", intensity},
        {"sizeOrRadius", radius},
        {"innerConeRadians", outerConeRadians * 0.5f},
        {"outerConeRadians", outerConeRadians},
        {"enabled", prim.IsActive()},
        {"animation", {
            {"transformTimeSampled", lightTransformAnimated},
            {"intensityTimeSampled", intensityAnimated},
            {"colorTimeSampled", colorAnimated},
            {"radiusTimeSampled", radiusAnimated},
            {"coneAngleTimeSampled", coneAngleAnimated},
            {"parametersTimeSampled", lightParametersAnimated},
            {"hasAnimation", lightParametersAnimated || lightTransformAnimated},
            {"animationDetectionImplemented", true},
            {"runtimePlaybackImplemented", lightParametersAnimated || lightTransformAnimated},
        }},
    };
}

std::string usdAttributeInterpolation(const pxr::UsdAttribute& attribute, std::string fallback = "vertex") {
    pxr::TfToken interpolation;
    if (attribute && attribute.GetMetadata(pxr::TfToken("interpolation"), &interpolation)) {
        return interpolation.GetString();
    }
    return fallback;
}

struct UsdVec2PrimvarData {
    pxr::VtArray<pxr::GfVec2f> values;
    pxr::VtArray<int> indices;
    std::string interpolation;
    bool valid = false;
    bool indexed = false;
};

struct UsdVec3PrimvarData {
    pxr::VtArray<pxr::GfVec3f> values;
    pxr::VtArray<int> indices;
    std::string interpolation;
    bool valid = false;
    bool indexed = false;
};

UsdVec2PrimvarData usdReadVec2Primvar(const pxr::UsdPrim& prim, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        pxr::UsdAttribute attribute = prim.GetAttribute(pxr::TfToken(name));
        if (!attribute) {
            continue;
        }
        UsdVec2PrimvarData out;
        out.valid = attribute.Get(&out.values, pxr::UsdTimeCode::Default()) && !out.values.empty();
        if (!out.valid) {
            continue;
        }
        out.interpolation = usdAttributeInterpolation(attribute);
        pxr::UsdAttribute indicesAttribute = prim.GetAttribute(pxr::TfToken(std::string(name) + ":indices"));
        out.indexed = indicesAttribute && indicesAttribute.Get(&out.indices, pxr::UsdTimeCode::Default()) && !out.indices.empty();
        return out;
    }
    return {};
}

UsdVec3PrimvarData usdReadVec3Primvar(const pxr::UsdPrim& prim, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        pxr::UsdAttribute attribute = prim.GetAttribute(pxr::TfToken(name));
        if (!attribute) {
            continue;
        }
        UsdVec3PrimvarData out;
        out.valid = attribute.Get(&out.values, pxr::UsdTimeCode::Default()) && !out.values.empty();
        if (!out.valid) {
            continue;
        }
        out.interpolation = usdAttributeInterpolation(attribute);
        pxr::UsdAttribute indicesAttribute = prim.GetAttribute(pxr::TfToken(std::string(name) + ":indices"));
        out.indexed = indicesAttribute && indicesAttribute.Get(&out.indices, pxr::UsdTimeCode::Default()) && !out.indices.empty();
        return out;
    }
    return {};
}

std::optional<size_t> usdPrimvarElementIndex(
    const std::string& interpolation,
    size_t pointIndex,
    size_t faceIndex,
    size_t faceVertexIndex,
    size_t elementCount) {
    if (elementCount == 0) {
        return std::nullopt;
    }
    if (interpolation == "faceVarying") {
        return faceVertexIndex < elementCount ? std::optional<size_t>(faceVertexIndex) : std::nullopt;
    }
    if (interpolation == "uniform") {
        return faceIndex < elementCount ? std::optional<size_t>(faceIndex) : std::nullopt;
    }
    if (interpolation == "constant") {
        return size_t{0};
    }
    return pointIndex < elementCount ? std::optional<size_t>(pointIndex) : std::nullopt;
}

std::optional<glm::vec2> usdReadVec2PrimvarValue(
    const UsdVec2PrimvarData& primvar,
    size_t pointIndex,
    size_t faceIndex,
    size_t faceVertexIndex) {
    if (!primvar.valid) {
        return std::nullopt;
    }
    const size_t sourceElementCount = primvar.indexed ? primvar.indices.size() : primvar.values.size();
    const std::optional<size_t> sourceElement = usdPrimvarElementIndex(
        primvar.interpolation,
        pointIndex,
        faceIndex,
        faceVertexIndex,
        sourceElementCount);
    if (!sourceElement.has_value()) {
        return std::nullopt;
    }
    size_t valueIndex = *sourceElement;
    if (primvar.indexed) {
        const int indexedValue = primvar.indices[*sourceElement];
        if (indexedValue < 0) {
            return std::nullopt;
        }
        valueIndex = static_cast<size_t>(indexedValue);
    }
    if (valueIndex >= primvar.values.size()) {
        return std::nullopt;
    }
    const pxr::GfVec2f& value = primvar.values[valueIndex];
    return glm::vec2{value[0], 1.0f - value[1]};
}

std::optional<glm::vec4> usdReadColorPrimvarValue(
    const UsdVec3PrimvarData& primvar,
    size_t pointIndex,
    size_t faceIndex,
    size_t faceVertexIndex) {
    if (!primvar.valid) {
        return std::nullopt;
    }
    const size_t sourceElementCount = primvar.indexed ? primvar.indices.size() : primvar.values.size();
    const std::optional<size_t> sourceElement = usdPrimvarElementIndex(
        primvar.interpolation,
        pointIndex,
        faceIndex,
        faceVertexIndex,
        sourceElementCount);
    if (!sourceElement.has_value()) {
        return std::nullopt;
    }
    size_t valueIndex = *sourceElement;
    if (primvar.indexed) {
        const int indexedValue = primvar.indices[*sourceElement];
        if (indexedValue < 0) {
            return std::nullopt;
        }
        valueIndex = static_cast<size_t>(indexedValue);
    }
    if (valueIndex >= primvar.values.size()) {
        return std::nullopt;
    }
    const pxr::GfVec3f& value = primvar.values[valueIndex];
    return glm::vec4{value[0], value[1], value[2], 1.0f};
}

nlohmann::json usdExpandedMeshPositionValues(
    const pxr::VtArray<int>& faceVertexCounts,
    const pxr::VtArray<int>& faceVertexIndices,
    const pxr::VtArray<pxr::GfVec3f>& points) {
    nlohmann::json values = nlohmann::json::array();
    size_t faceVertexOffset = 0;
    for (size_t faceIndex = 0; faceIndex < faceVertexCounts.size(); ++faceIndex) {
        const int faceVertexCount = faceVertexCounts[faceIndex];
        if (faceVertexCount < 3 || faceVertexOffset + static_cast<size_t>(faceVertexCount) > faceVertexIndices.size()) {
            faceVertexOffset += static_cast<size_t>(std::max(faceVertexCount, 0));
            continue;
        }
        for (int tri = 1; tri + 1 < faceVertexCount; ++tri) {
            const int corners[3] = {0, tri, tri + 1};
            bool validTriangle = true;
            glm::vec3 triangle[3]{};
            for (int i = 0; i < 3; ++i) {
                const size_t faceVertexIndex = faceVertexOffset + static_cast<size_t>(corners[i]);
                const int pointIndex = faceVertexIndices[faceVertexIndex];
                if (pointIndex < 0 || static_cast<size_t>(pointIndex) >= points.size()) {
                    validTriangle = false;
                    break;
                }
                triangle[i] = glmVec3FromUsd(points[static_cast<size_t>(pointIndex)]);
            }
            if (!validTriangle) {
                continue;
            }
            for (const glm::vec3& point : triangle) {
                values.push_back(point.x);
                values.push_back(point.y);
                values.push_back(point.z);
            }
        }
        faceVertexOffset += static_cast<size_t>(faceVertexCount);
    }
    return values;
}

std::vector<std::string> usdMeshFaceMaterialPaths(
    const pxr::UsdPrim& prim,
    size_t faceCount,
    const std::string& meshMaterialPath,
    nlohmann::json& subsetReport,
    std::vector<std::string>& warnings) {
    std::vector<std::string> faceMaterialPaths(faceCount, meshMaterialPath);
    subsetReport = nlohmann::json::array();
    for (const pxr::UsdPrim& child : prim.GetChildren()) {
        if (!child || child.GetTypeName().GetString() != "GeomSubset") {
            continue;
        }
        pxr::VtArray<int> faceIndices;
        pxr::UsdAttribute indicesAttribute = child.GetAttribute(pxr::TfToken("indices"));
        if (!indicesAttribute || !indicesAttribute.Get(&faceIndices, pxr::UsdTimeCode::Default()) || faceIndices.empty()) {
            subsetReport.push_back({
                {"path", child.GetPath().GetString()},
                {"decoded", false},
                {"reason", "missing_indices"},
            });
            continue;
        }
        const std::string subsetMaterialPath = usdMaterialBindingTargetPath(child);
        if (subsetMaterialPath.empty()) {
            subsetReport.push_back({
                {"path", child.GetPath().GetString()},
                {"decoded", false},
                {"reason", "missing_material_binding"},
                {"faceCount", faceIndices.size()},
            });
            continue;
        }
        size_t appliedCount = 0;
        size_t outOfRangeCount = 0;
        for (int faceIndex : faceIndices) {
            if (faceIndex < 0 || static_cast<size_t>(faceIndex) >= faceMaterialPaths.size()) {
                ++outOfRangeCount;
                continue;
            }
            faceMaterialPaths[static_cast<size_t>(faceIndex)] = subsetMaterialPath;
            ++appliedCount;
        }
        if (outOfRangeCount > 0) {
            warnings.push_back("USD material subset " + child.GetPath().GetString() + " referenced out-of-range face indices.");
        }
        subsetReport.push_back({
            {"path", child.GetPath().GetString()},
            {"decoded", true},
            {"materialBindingPath", subsetMaterialPath},
            {"faceCount", faceIndices.size()},
            {"appliedFaceCount", appliedCount},
            {"outOfRangeFaceCount", outOfRangeCount},
        });
    }
    return faceMaterialPaths;
}

std::optional<MeshAsset> decodeUsdMeshAsset(
    const pxr::UsdGeomMesh& usdMesh,
    std::string_view fallbackName,
    pxr::UsdGeomXformCache* xformCache,
    nlohmann::json& meshReport,
    std::vector<std::string>& warnings,
    const glm::mat4* stageConversion = nullptr) {
    const pxr::UsdPrim prim = usdMesh.GetPrim();
    pxr::VtArray<pxr::GfVec3f> points;
    pxr::VtArray<int> faceVertexCounts;
    pxr::VtArray<int> faceVertexIndices;
    if (!usdMesh.GetPointsAttr().Get(&points, pxr::UsdTimeCode::Default()) || points.empty()) {
        meshReport = {{"primPath", prim.GetPath().GetString()}, {"decoded", false}, {"reason", "missing_points"}};
        return std::nullopt;
    }
    if (!usdMesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts, pxr::UsdTimeCode::Default()) || faceVertexCounts.empty()) {
        meshReport = {{"primPath", prim.GetPath().GetString()}, {"decoded", false}, {"reason", "missing_face_vertex_counts"}};
        return std::nullopt;
    }
    if (!usdMesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices, pxr::UsdTimeCode::Default()) || faceVertexIndices.empty()) {
        meshReport = {{"primPath", prim.GetPath().GetString()}, {"decoded", false}, {"reason", "missing_face_vertex_indices"}};
        return std::nullopt;
    }

    pxr::VtArray<pxr::GfVec3f> authoredNormals;
    const bool hasAuthoredNormals = usdMesh.GetNormalsAttr().Get(&authoredNormals, pxr::UsdTimeCode::Default()) && !authoredNormals.empty();
    const std::string interpolation = usdMesh.GetNormalsInterpolation().GetString();
    const bool normalsAreVertexIndexed = hasAuthoredNormals && authoredNormals.size() == points.size() && interpolation != "faceVarying";
    const bool normalsAreFaceVarying = hasAuthoredNormals && authoredNormals.size() == faceVertexIndices.size();
    const std::string materialBindingPath = usdMaterialBindingTargetPath(prim);
    const UsdVec2PrimvarData uv0 = usdReadVec2Primvar(prim, {"primvars:st", "primvars:UVMap"});
    const UsdVec2PrimvarData uv1 = usdReadVec2Primvar(prim, {"primvars:st1", "primvars:uv1", "primvars:UVMap1"});
    const UsdVec3PrimvarData displayColor = usdReadVec3Primvar(prim, {"primvars:displayColor", "displayColor"});
    nlohmann::json materialSubsetReport = nlohmann::json::array();
    const std::vector<std::string> faceMaterialPaths = usdMeshFaceMaterialPaths(
        prim,
        faceVertexCounts.size(),
        materialBindingPath,
        materialSubsetReport,
        warnings);

    MeshAsset out;
    out.name = prim.GetName().GetString();
    if (out.name.empty()) {
        out.name = std::string(fallbackName.empty() ? "UsdMesh" : fallbackName);
    }

    size_t faceVertexOffset = 0;
    size_t skippedFaces = 0;
    bool generatedAnyNormals = false;
    bool decodedAnyUv0 = false;
    bool decodedAnyUv1 = false;
    bool decodedAnyVertexColor = false;
    bool generatedTangents = false;
    std::vector<std::string> primitiveMaterialBindingPaths;
    auto beginPrimitive = [&](const std::string& primitiveMaterialPath) {
        MeshPrimitiveAsset primitive;
        primitive.firstVertex = static_cast<uint32_t>(out.vertices.size());
        primitive.firstIndex = static_cast<uint32_t>(out.indices.size());
        primitive.alphaMode = kMaterialAlphaModeOpaque;
        primitive.alphaCutoff = 0.5f;
        out.primitives.push_back(primitive);
        primitiveMaterialBindingPaths.push_back(primitiveMaterialPath);
    };
    auto closePrimitive = [&]() {
        if (out.primitives.empty()) {
            return;
        }
        MeshPrimitiveAsset& primitive = out.primitives.back();
        primitive.vertexCount = static_cast<uint32_t>(out.vertices.size()) - primitive.firstVertex;
        primitive.indexCount = static_cast<uint32_t>(out.indices.size()) - primitive.firstIndex;
        if (primitive.vertexCount == 0u || primitive.indexCount == 0u) {
            out.primitives.pop_back();
            if (!primitiveMaterialBindingPaths.empty()) {
                primitiveMaterialBindingPaths.pop_back();
            }
        }
    };
    for (size_t faceIndex = 0; faceIndex < faceVertexCounts.size(); ++faceIndex) {
        const int faceVertexCount = faceVertexCounts[faceIndex];
        if (faceVertexCount < 3 || faceVertexOffset + static_cast<size_t>(faceVertexCount) > faceVertexIndices.size()) {
            faceVertexOffset += static_cast<size_t>(std::max(faceVertexCount, 0));
            ++skippedFaces;
            continue;
        }
        const std::string& faceMaterialPath = faceIndex < faceMaterialPaths.size() ? faceMaterialPaths[faceIndex] : materialBindingPath;
        if (out.primitives.empty() || primitiveMaterialBindingPaths.back() != faceMaterialPath) {
            closePrimitive();
            beginPrimitive(faceMaterialPath);
        }

        for (int tri = 1; tri + 1 < faceVertexCount; ++tri) {
            const int corners[3] = {0, tri, tri + 1};
            const size_t triangleVertexBase = out.vertices.size();
            bool triangleMissingNormal = false;
            bool triangleHasUv0 = false;
            for (int corner : corners) {
                const size_t faceVertexIndex = faceVertexOffset + static_cast<size_t>(corner);
                const int pointIndex = faceVertexIndices[faceVertexIndex];
                if (pointIndex < 0 || static_cast<size_t>(pointIndex) >= points.size()) {
                    triangleMissingNormal = true;
                    continue;
                }
                MeshVertex vertex;
                vertex.position = glmVec3FromUsd(points[static_cast<size_t>(pointIndex)]);
                if (normalsAreFaceVarying && faceVertexIndex < authoredNormals.size()) {
                    vertex.normal = normalizedOrFallback(glmVec3FromUsd(authoredNormals[faceVertexIndex]), vertex.normal);
                } else if (normalsAreVertexIndexed && static_cast<size_t>(pointIndex) < authoredNormals.size()) {
                    vertex.normal = normalizedOrFallback(glmVec3FromUsd(authoredNormals[static_cast<size_t>(pointIndex)]), vertex.normal);
                } else {
                    triangleMissingNormal = true;
                }
                if (std::optional<glm::vec2> texcoord = usdReadVec2PrimvarValue(uv0, static_cast<size_t>(pointIndex), faceIndex, faceVertexIndex)) {
                    vertex.texcoord = *texcoord;
                    decodedAnyUv0 = true;
                    triangleHasUv0 = true;
                }
                if (std::optional<glm::vec2> texcoord1 = usdReadVec2PrimvarValue(uv1, static_cast<size_t>(pointIndex), faceIndex, faceVertexIndex)) {
                    vertex.texcoord1 = *texcoord1;
                    decodedAnyUv1 = true;
                }
                if (std::optional<glm::vec4> color = usdReadColorPrimvarValue(displayColor, static_cast<size_t>(pointIndex), faceIndex, faceVertexIndex)) {
                    vertex.color = *color;
                    decodedAnyVertexColor = true;
                }
                out.vertices.push_back(vertex);
                out.indices.push_back(static_cast<uint32_t>(out.vertices.size() - 1u));
            }
            if (out.vertices.size() - triangleVertexBase != 3u) {
                while (out.vertices.size() > triangleVertexBase) {
                    out.vertices.pop_back();
                }
                while (out.indices.size() > triangleVertexBase) {
                    out.indices.pop_back();
                }
                ++skippedFaces;
                continue;
            }
            if (triangleMissingNormal) {
                const glm::vec3 p0 = out.vertices[triangleVertexBase + 0u].position;
                const glm::vec3 p1 = out.vertices[triangleVertexBase + 1u].position;
                const glm::vec3 p2 = out.vertices[triangleVertexBase + 2u].position;
                const glm::vec3 normal = normalizedOrFallback(glm::cross(p1 - p0, p2 - p0), glm::vec3{0.0f, 1.0f, 0.0f});
                out.vertices[triangleVertexBase + 0u].normal = normal;
                out.vertices[triangleVertexBase + 1u].normal = normal;
                out.vertices[triangleVertexBase + 2u].normal = normal;
                generatedAnyNormals = true;
            }
            if (triangleHasUv0) {
                const glm::vec3 p0 = out.vertices[triangleVertexBase + 0u].position;
                const glm::vec3 p1 = out.vertices[triangleVertexBase + 1u].position;
                const glm::vec3 p2 = out.vertices[triangleVertexBase + 2u].position;
                const glm::vec2 uv00 = out.vertices[triangleVertexBase + 0u].texcoord;
                const glm::vec2 uv01 = out.vertices[triangleVertexBase + 1u].texcoord;
                const glm::vec2 uv02 = out.vertices[triangleVertexBase + 2u].texcoord;
                const glm::vec3 edge1 = p1 - p0;
                const glm::vec3 edge2 = p2 - p0;
                const glm::vec2 duv1 = uv01 - uv00;
                const glm::vec2 duv2 = uv02 - uv00;
                const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
                if (std::abs(determinant) > 1.0e-8f) {
                    const float invDeterminant = 1.0f / determinant;
                    const glm::vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) * invDeterminant;
                    const glm::vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * invDeterminant;
                    const glm::vec3 normal = normalizedOrFallback(
                        out.vertices[triangleVertexBase + 0u].normal +
                            out.vertices[triangleVertexBase + 1u].normal +
                            out.vertices[triangleVertexBase + 2u].normal,
                        glm::vec3{0.0f, 1.0f, 0.0f});
                    const float sign = glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
                    const glm::vec4 encodedTangent = normalizedTangentOrFallback(tangent, sign);
                    out.vertices[triangleVertexBase + 0u].tangent = encodedTangent;
                    out.vertices[triangleVertexBase + 1u].tangent = encodedTangent;
                    out.vertices[triangleVertexBase + 2u].tangent = encodedTangent;
                    generatedTangents = true;
                }
            }
        }
        faceVertexOffset += static_cast<size_t>(faceVertexCount);
    }

    closePrimitive();
    if (out.vertices.empty() || out.indices.empty()) {
        meshReport = {{"primPath", prim.GetPath().GetString()}, {"decoded", false}, {"reason", "no_triangles_decoded"}};
        return std::nullopt;
    }
    if (skippedFaces > 0) {
        warnings.push_back("USD mesh cook skipped invalid or unsupported faces for prim " + prim.GetPath().GetString() + ".");
    }
    if (generatedAnyNormals) {
        warnings.push_back("USD mesh cook generated fallback face normals for prim " + prim.GetPath().GetString() + ".");
    }
    nlohmann::json pointAnimation = nlohmann::json::object({
        {"decoded", false},
        {"runtimePlaybackImplemented", false},
    });
    std::vector<double> pointTimeSamples;
    const pxr::UsdAttribute pointsAttr = usdMesh.GetPointsAttr();
    if (pointsAttr && pointsAttr.GetTimeSamples(&pointTimeSamples) && pointTimeSamples.size() > 1u) {
        nlohmann::json times = nlohmann::json::array();
        nlohmann::json values = nlohmann::json::array();
        for (double timeCode : pointTimeSamples) {
            pxr::VtArray<pxr::GfVec3f> sampledPoints;
            if (!pointsAttr.Get(&sampledPoints, pxr::UsdTimeCode(timeCode)) || sampledPoints.empty()) {
                continue;
            }
            nlohmann::json expanded = usdExpandedMeshPositionValues(faceVertexCounts, faceVertexIndices, sampledPoints);
            if (expanded.size() != out.vertices.size() * 3u) {
                continue;
            }
            times.push_back(timeCode);
            values.push_back(std::move(expanded));
        }
        pointAnimation = {
            {"decoded", times.size() > 1u && values.size() == times.size()},
            {"targetPath", "meshVertexPositions"},
            {"timeCodes", times},
            {"values", values},
            {"keyframeCount", times.size()},
            {"expandedVertexCount", out.vertices.size()},
            {"runtimePlaybackImplemented", times.size() > 1u && values.size() == times.size()},
        };
    }
    meshReport = {
        {"primPath", prim.GetPath().GetString()},
        {"name", out.name},
        {"transform", usdPrimTransformJson(prim, xformCache, stageConversion, false)},
        {"decoded", true},
        {"pointCount", points.size()},
        {"faceCount", faceVertexCounts.size()},
        {"vertexCount", out.vertices.size()},
        {"indexCount", out.indices.size()},
        {"primitiveCount", out.primitives.size()},
        {"skippedFaceCount", skippedFaces},
        {"generatedNormals", generatedAnyNormals},
        {"normalInterpolation", interpolation},
        {"uv0Decoded", decodedAnyUv0},
        {"uv0Interpolation", uv0.valid ? uv0.interpolation : std::string{}},
        {"uv0Indexed", uv0.indexed},
        {"uv1Decoded", decodedAnyUv1},
        {"uv1Interpolation", uv1.valid ? uv1.interpolation : std::string{}},
        {"uv1Indexed", uv1.indexed},
        {"vertexColorDecoded", decodedAnyVertexColor},
        {"vertexColorInterpolation", displayColor.valid ? displayColor.interpolation : std::string{}},
        {"vertexColorIndexed", displayColor.indexed},
        {"pointAnimation", pointAnimation},
        {"tangentsGenerated", generatedTangents},
        {"materialBound", usdPrimHasAuthoredMaterialBinding(prim)},
        {"materialBindingPath", materialBindingPath},
        {"materialSubsetCount", materialSubsetReport.size()},
        {"materialSubsets", materialSubsetReport},
        {"primitiveMaterialBindingPaths", primitiveMaterialBindingPaths},
    };
    return out;
}
#endif

UsdMaterialShaderNetworkData loadUsdMaterialShaderNetwork(
    const std::filesystem::path& sourcePath,
    std::string_view materialPath,
    std::string_view materialName) {
#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
    pxr::UsdStageRefPtr stage = openUsdStageForImport(sourcePath);
    if (!stage) {
        return defaultUsdMaterialShaderNetworkData(sourcePath, materialPath, materialName);
    }
    return loadUsdMaterialShaderNetworkOpenUsd(sourcePath, stage, materialPath, materialName);
#else
    return defaultUsdMaterialShaderNetworkData(sourcePath, materialPath, materialName);
#endif
}

std::vector<UsdMaterialShaderNetworkData> loadUsdMaterialShaderNetworks(
    const std::filesystem::path& sourcePath,
    const std::vector<UsdMaterialShaderNetworkRequest>& materialRequests,
    const std::function<void(size_t, size_t, const std::string&)>& progress) {
    std::vector<UsdMaterialShaderNetworkData> out;
    out.reserve(materialRequests.size());
#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
    pxr::UsdStageRefPtr stage = openUsdStageForImport(sourcePath);
    std::unordered_map<std::string, size_t> firstRequestBySourceNetworkKey;
    for (size_t i = 0; i < materialRequests.size(); ++i) {
        const UsdMaterialShaderNetworkRequest& request = materialRequests[i];
        if (progress) {
            progress(i, materialRequests.size(), request.materialPath);
        }
        if (stage) {
            const pxr::UsdPrim materialPrim = stage->GetPrimAtPath(pxr::SdfPath(request.materialPath));
            const std::string sourceNetworkKey = usdMaterialSourceNetworkKeyOpenUsd(materialPrim);
            if (!sourceNetworkKey.empty()) {
                const auto sharedIt = firstRequestBySourceNetworkKey.find(sourceNetworkKey);
                if (sharedIt != firstRequestBySourceNetworkKey.end() && sharedIt->second < out.size()) {
                    UsdMaterialShaderNetworkData shared = out[sharedIt->second];
                    shared.material.name = request.materialName;
                    shared.diagnostics["sourceMaterialPath"] = request.materialPath;
                    shared.diagnostics["sourceNetworkDeduplicated"] = true;
                    shared.diagnostics["sharedSourceMaterialPath"] = materialRequests[sharedIt->second].materialPath;
                    shared.diagnostics["sharedRequestIndex"] = sharedIt->second;
                    shared.diagnostics["sourceNetworkKey"] = sourceNetworkKey;
                    out.push_back(std::move(shared));
                    continue;
                }
                firstRequestBySourceNetworkKey.emplace(sourceNetworkKey, i);
            }
            out.push_back(loadUsdMaterialShaderNetworkOpenUsd(sourcePath, stage, request.materialPath, request.materialName));
            if (!sourceNetworkKey.empty()) {
                out.back().diagnostics["sourceNetworkDeduplicated"] = false;
                out.back().diagnostics["sourceNetworkKey"] = sourceNetworkKey;
            }
        } else {
            out.push_back(defaultUsdMaterialShaderNetworkData(sourcePath, request.materialPath, request.materialName));
        }
    }
#else
    for (size_t i = 0; i < materialRequests.size(); ++i) {
        const UsdMaterialShaderNetworkRequest& request = materialRequests[i];
        if (progress) {
            progress(i, materialRequests.size(), request.materialPath);
        }
        out.push_back(defaultUsdMaterialShaderNetworkData(sourcePath, request.materialPath, request.materialName));
    }
#endif
    if (progress) {
        progress(materialRequests.size(), materialRequests.size(), std::string{});
    }
    return out;
}

UsdStageImportData loadUsdStageMetadata(const std::filesystem::path& sourcePath) {
    UsdStageImportData out;
#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
    pxr::UsdStageRefPtr stage = openUsdStageForImport(sourcePath);
    if (!stage) {
        out.errors.push_back("OpenUSD failed to open the USD/USDZ stage.");
        out.diagnostics = {
            {"schema", "UsdStageImportDiagnosticsV1"},
            {"parser", "OpenUSD"},
            {"supported", false},
            {"sourcePath", sourcePath.generic_string()},
            {"errors", out.errors},
        };
        return out;
    }

    out.supported = true;
    out.metersPerUnit = pxr::UsdGeomGetStageMetersPerUnit(stage);
    out.upAxis = pxr::UsdGeomGetStageUpAxis(stage).GetString();
    const glm::mat4 stageConversion = usdStageConversionMatrix(out.metersPerUnit, out.upAxis);
    const std::string pseudoRootPath = stage->GetPseudoRoot().GetPath().GetString();
    // Stage-level animation time range. A stage with time-sampled data authors a
    // start/end time code and a frame rate; static stages leave these at the USD
    // defaults. Transform samples are decoded into the shared runtime animation
    // clip format; deforming mesh points remain default-time geometry with
    // diagnostics because the current runtime clip schema has no mesh-point track.
    out.hasTimeSamples = stage->HasAuthoredTimeCodeRange();
    out.timeCodesPerSecond = stage->GetTimeCodesPerSecond();
    if (out.hasTimeSamples) {
        out.startTimeCode = stage->GetStartTimeCode();
        out.endTimeCode = stage->GetEndTimeCode();
    }
    // Stage-level composition: count sublayers contributed by the root layer
    // stack. OpenUSD has already composed references/payloads/sublayers/variants
    // into the traversable stage, so traversal sees the fully-composed result;
    // we additionally surface the authored composition arcs as diagnostics so the
    // import provenance records what composition the stage relied on, even though
    // the engine consumes the already-composed prims rather than re-resolving arcs.
    {
        const pxr::SdfLayerHandle rootLayer = stage->GetRootLayer();
        if (rootLayer) {
            out.sublayerCount = rootLayer->GetSubLayerPaths().size();
        }
    }
    // Prototypes/instances: USD shares a single prototype prim tree across all
    // native instances of an instanceable prim, so the engine can share geometry
    // rather than duplicating it per instance. The default traversal does not
    // descend into prototypes, so we enumerate them explicitly and count the
    // mesh prims each prototype tree contains. This records that instanced
    // geometry is backed by a shared prototype (efficient import provenance);
    // the instance-to-prototype mapping is also surfaced per instance prim below.
    {
        const std::vector<pxr::UsdPrim> prototypes = stage->GetPrototypes();
        out.prototypeCount = prototypes.size();
        for (const pxr::UsdPrim& prototype : prototypes) {
            if (!prototype) {
                continue;
            }
            // The instanced prim's reference target may itself be a Mesh, in which
            // case the prototype root prim is the mesh; otherwise the mesh prims
            // live among the prototype's descendants. Count both so a single-mesh
            // instanceable reference is reported correctly.
            if (prototype.IsA<pxr::UsdGeomMesh>()) {
                ++out.instancedPrototypeMeshPrimCount;
            }
            for (const pxr::UsdPrim& child : prototype.GetAllDescendants()) {
                if (child && child.IsA<pxr::UsdGeomMesh>()) {
                    ++out.instancedPrototypeMeshPrimCount;
                }
            }
        }
    }
    pxr::UsdGeomXformCache xformCache(pxr::UsdTimeCode::Default());
    std::unordered_map<std::string, size_t> primIndexByPath;
    nlohmann::json usdTransformAnimationChannelsJson = nlohmann::json::array();
    uint32_t usdDecodedTransformAnimationChannelCount = 0;
    uint32_t usdDecodedTransformAnimationKeyframeCount = 0;
    uint32_t usdDecodedCameraLightAnimationChannelCount = 0;
    uint32_t usdDecodedCameraLightAnimationKeyframeCount = 0;
    size_t primCount = 0;
    size_t xformableCount = 0;
    size_t materialPrimCount = 0;
    for (const pxr::UsdPrim& prim : usdStageImportPrimRange(stage)) {
        if (!prim) {
            continue;
        }
        const std::string path = prim.GetPath().GetString();
        const std::string parentPath = prim.GetParent() ? prim.GetParent().GetPath().GetString() : std::string{};
        const std::string typeName = prim.GetTypeName().GetString();
        const bool isMesh = prim.IsA<pxr::UsdGeomMesh>();
        const bool isCamera = prim.IsA<pxr::UsdGeomCamera>();
        const bool isLight = usdPrimLooksLikeLight(prim);
        const bool hasMaterialBinding = usdPrimHasAuthoredMaterialBinding(prim);
        if (isMesh) {
            ++out.meshCount;
        }
        if (isCamera) {
            ++out.cameraCount;
            out.cameras.push_back(usdCameraRuntimeJson(pxr::UsdGeomCamera(prim), &xformCache, &stageConversion));
        }
        if (isLight) {
            ++out.lightCount;
            out.lights.push_back(usdLightRuntimeJson(prim, &xformCache, &stageConversion));
        }
        if (hasMaterialBinding) {
            ++out.materialBindingCount;
        }
        if (typeName == "Material") {
            ++materialPrimCount;
        }
        const pxr::UsdGeomXformable xformablePrim(prim);
        const bool isXformable = static_cast<bool>(xformablePrim);
        if (isXformable) {
            ++xformableCount;
        }
        const bool isRootPrim = parentPath == pseudoRootPath;

        // Detect time-sampled (animated) transforms. UsdGeomXformable reports
        // whether any of its xformOps carry more than one authored time sample;
        // we surface this as a per-prim diagnostic so animation provenance is
        // visible even though runtime USD transform animation playback is not
        // yet wired into the engine animation system.
        bool transformIsTimeSampled = false;
        std::vector<double> xformTimeSamples;
        if (isXformable) {
            if (xformablePrim.GetTimeSamples(&xformTimeSamples) && xformTimeSamples.size() > 1u) {
                transformIsTimeSampled = true;
                ++out.timeSampledTransformPrimCount;
                nlohmann::json primChannels = usdTransformAnimationChannels(
                    prim,
                    static_cast<int>(primCount),
                    xformTimeSamples,
                    out.timeCodesPerSecond,
                    &stageConversion,
                    isRootPrim,
                    usdDecodedTransformAnimationChannelCount,
                    usdDecodedTransformAnimationKeyframeCount);
                for (nlohmann::json& channel : primChannels) {
                    usdTransformAnimationChannelsJson.push_back(std::move(channel));
                }
            }
        }
        if (isCamera) {
            nlohmann::json primChannels = usdCameraParameterAnimationChannels(
                pxr::UsdGeomCamera(prim),
                static_cast<int>(primCount),
                out.timeCodesPerSecond,
                usdDecodedCameraLightAnimationChannelCount,
                usdDecodedCameraLightAnimationKeyframeCount);
            for (nlohmann::json& channel : primChannels) {
                usdTransformAnimationChannelsJson.push_back(std::move(channel));
            }
        } else if (isLight) {
            nlohmann::json primChannels = usdLightParameterAnimationChannels(
                prim,
                static_cast<int>(primCount),
                out.timeCodesPerSecond,
                usdDecodedCameraLightAnimationChannelCount,
                usdDecodedCameraLightAnimationKeyframeCount);
            for (nlohmann::json& channel : primChannels) {
                usdTransformAnimationChannelsJson.push_back(std::move(channel));
            }
        }
        // Deforming geometry (animated points) and animated visibility are also
        // surfaced per prim. These are detected from authored attribute time
        // samples; point deformation is imported at the default time and reported
        // because runtime mesh-point tracks are not part of the shared clip schema.
        bool pointsAreTimeSampled = false;
        bool visibilityIsTimeSampled = false;
        if (isMesh) {
            const pxr::UsdAttribute pointsAttr = pxr::UsdGeomMesh(prim).GetPointsAttr();
            if (pointsAttr && pointsAttr.GetNumTimeSamples() > 1u) {
                pointsAreTimeSampled = true;
                ++out.timeSampledPointsPrimCount;
            }
        }
        if (isXformable) {
            const pxr::UsdAttribute visibilityAttr = pxr::UsdGeomImageable(prim).GetVisibilityAttr();
            if (visibilityAttr && visibilityAttr.GetNumTimeSamples() > 1u) {
                visibilityIsTimeSampled = true;
                ++out.timeSampledVisibilityPrimCount;
            }
        }

        // Per-prim composition arcs. OpenUSD has already composed the stage, so
        // traversal sees the composed result; we surface which composition arcs a
        // prim authored (references, payloads, variant sets, instancing) so the
        // import provenance records the composition the prim relied on. These are
        // detected from the prim's authored composition, not re-resolved.
        bool primHasReferences = false;
        bool primHasPayloads = false;
        size_t primVariantSetCount = 0;
        bool primIsInstance = false;
        std::string primPrototypePath;
        {
            primHasReferences = prim.HasAuthoredReferences();
            primHasPayloads = prim.HasAuthoredPayloads();
            const std::vector<std::string> variantSetNames = prim.GetVariantSets().GetNames();
            primVariantSetCount = variantSetNames.size();
            primIsInstance = prim.IsInstance();
            if (primHasReferences) {
                ++out.referencePrimCount;
            }
            if (primHasPayloads) {
                ++out.payloadPrimCount;
            }
            if (primVariantSetCount > 0) {
                ++out.variantSetPrimCount;
                out.variantSetCount += primVariantSetCount;
            }
            if (primIsInstance) {
                ++out.instancePrimCount;
                // Record which shared prototype tree backs this instance so the
                // instance-to-prototype mapping is visible (efficient shared-geometry
                // import provenance rather than per-instance duplication).
                const pxr::UsdPrim prototype = prim.GetPrototype();
                if (prototype) {
                    primPrototypePath = prototype.GetPath().GetString();
                }
            }
        }

        // Preserve visibility, purpose, and active state. Visibility uses the
        // computed (inherited) value so a prim under an invisible ancestor is
        // reported invisible; purpose is the authored/inherited imageable purpose
        // (default/render/proxy/guide). Active state is read from the prim. These
        // are surfaced as diagnostics and consumed by USD runtime placement:
        // invisible/inactive/proxy/guide prims disable renderable mesh/light
        // state while retaining the hierarchy for editor inspection.
        const bool primIsActive = prim.IsActive();
        std::string primVisibility = "inherited";
        std::string primPurpose = "default";
        if (isXformable) {
            const pxr::UsdGeomImageable imageable(prim);
            if (imageable) {
                const pxr::TfToken computedVis = imageable.ComputeVisibility(pxr::UsdTimeCode::Default());
                primVisibility = computedVis.GetString();
                const pxr::TfToken computedPurpose = imageable.ComputePurpose();
                primPurpose = computedPurpose.GetString();
            }
        }
        const bool primIsInvisible = primVisibility == "invisible";
        if (primIsInvisible) {
            ++out.invisiblePrimCount;
        }
        if (!primIsActive) {
            ++out.inactivePrimCount;
        }
        if (primPurpose == "guide") {
            ++out.guidePurposePrimCount;
        } else if (primPurpose == "proxy") {
            ++out.proxyPurposePrimCount;
        } else if (primPurpose == "render") {
            ++out.renderPurposePrimCount;
        }

        nlohmann::json primJson = {
            {"index", primCount},
            {"path", path},
            {"name", prim.GetName().GetString()},
            {"parentPath", parentPath},
            {"typeName", typeName},
            {"active", primIsActive},
            {"defined", prim.IsDefined()},
            {"abstract", prim.IsAbstract()},
            {"visibility", primVisibility},
            {"visible", !primIsInvisible},
            {"purpose", primPurpose},
            {"xformable", isXformable},
            {"transformTimeSampled", transformIsTimeSampled},
            {"pointsTimeSampled", pointsAreTimeSampled},
            {"visibilityTimeSampled", visibilityIsTimeSampled},
            {"hasReferences", primHasReferences},
            {"hasPayloads", primHasPayloads},
            {"variantSetCount", primVariantSetCount},
            {"instance", primIsInstance},
            {"prototypePath", primPrototypePath},
            {"transform", usdPrimTransformJson(prim, &xformCache, &stageConversion, isRootPrim)},
            {"mesh", isMesh},
            {"camera", isCamera},
            {"light", isLight},
            {"materialPrim", typeName == "Material"},
            {"materialBound", hasMaterialBinding},
        };
        if (isMesh) {
            primJson["meshMetadata"] = usdMeshMetadataJson(pxr::UsdGeomMesh(prim));
        }
        if (parentPath == stage->GetPseudoRoot().GetPath().GetString()) {
            out.rootPrims.push_back(path);
        }
        primIndexByPath.emplace(path, primCount);
        out.prims.push_back(std::move(primJson));
        ++primCount;
    }

    const bool isUsdz = lowerString(sourcePath.extension().string()) == ".usdz";
    if (isUsdz) {
        out.warnings.push_back("USDZ package dependencies are inspected during stage metadata import; packaged texture extraction and native texture cooking run in the package texture cook stage when import settings allow textures.");
    }
    if (out.meshCount > 0) {
        out.warnings.push_back("USD mesh topology metadata was preserved; native .rtmesh cooking runs in the OpenUSD runtime mesh cook stage when topology decodes successfully.");
    }
    if (out.materialBindingCount > 0 || materialPrimCount > 0) {
        out.warnings.push_back("USD material binding metadata was preserved; bound-material .rtmaterial cook, PreviewSurface factor conversion, texture reference diagnostics, external texture cooking, and shader texture binding run in later USD material cook stages.");
    }

    out.decodedTransformAnimationChannelCount = usdDecodedTransformAnimationChannelCount;
    out.decodedTransformAnimationKeyframeCount = usdDecodedTransformAnimationKeyframeCount;
    out.decodedCameraLightAnimationChannelCount = usdDecodedCameraLightAnimationChannelCount;
    out.decodedCameraLightAnimationKeyframeCount = usdDecodedCameraLightAnimationKeyframeCount;
    if (!usdTransformAnimationChannelsJson.empty()) {
        double clipStart = std::numeric_limits<double>::max();
        double clipEnd = 0.0;
        for (const nlohmann::json& channel : usdTransformAnimationChannelsJson) {
            const nlohmann::json decodedTrack = channel.value("decodedTrack", nlohmann::json::object());
            const nlohmann::json times = decodedTrack.value("times", nlohmann::json::array());
            if (!times.empty()) {
                clipStart = std::min(clipStart, times.front().get<double>());
                clipEnd = std::max(clipEnd, times.back().get<double>());
            }
        }
        if (clipStart == std::numeric_limits<double>::max()) {
            clipStart = 0.0;
        }
        const std::string animationName = safeStem(sourcePath.stem().string() + "_UsdTransformAnimation");
        out.animations.push_back({
            {"schema", "UsdTransformAnimationClipV1"},
            {"sourceFormat", "USD"},
            {"name", animationName},
            {"clip", {
                {"startTime", clipStart},
                {"endTime", clipEnd},
                {"duration", std::max(0.0, clipEnd - clipStart)},
                {"timeCodesPerSecond", out.timeCodesPerSecond},
                {"sourceStartTimeCode", out.startTimeCode},
                {"sourceEndTimeCode", out.endTimeCode},
            }},
            {"channelCount", usdTransformAnimationChannelsJson.size()},
            {"decodedChannelCount", usdDecodedTransformAnimationChannelCount + usdDecodedCameraLightAnimationChannelCount},
            {"decodedKeyframeCount", usdDecodedTransformAnimationKeyframeCount + usdDecodedCameraLightAnimationKeyframeCount},
            {"decodedTransformChannelCount", usdDecodedTransformAnimationChannelCount},
            {"decodedCameraLightParameterChannelCount", usdDecodedCameraLightAnimationChannelCount},
            {"channels", usdTransformAnimationChannelsJson},
            {"runtimeSupport", "decoded_usd_transform_camera_light_keyframes_runtime_playback_supported"},
        });
    }

    // Engine runtime convention is Y-up with 1 unit == 1 meter. Surface a
    // conversion diagnostic when the stage authored a different metric so the
    // import provenance records the scale/axis remap that downstream placement
    // applies (and so unexpected unit/axis mismatches are debuggable).
    const bool upAxisIsZ = out.upAxis == "Z";
    const bool metersPerUnitAuthored = out.metersPerUnit > 0.0;
    const bool metersPerUnitNonMeter = metersPerUnitAuthored && std::abs(out.metersPerUnit - 1.0) > 1e-6;
    if (upAxisIsZ) {
        out.warnings.push_back("USD stage upAxis is Z; import records a Z-up to engine Y-up axis conversion (rotate -90 deg about X).");
    }
    if (metersPerUnitNonMeter) {
        out.warnings.push_back("USD stage metersPerUnit is not 1.0; import records a uniform scale to engine meter units (scale = metersPerUnit).");
    }

    const size_t timeSampledPrimCount = out.timeSampledTransformPrimCount +
        out.timeSampledPointsPrimCount + out.timeSampledVisibilityPrimCount;
    const bool stageHasAnimation = out.hasTimeSamples || timeSampledPrimCount > 0;
    if (stageHasAnimation) {
        out.warnings.push_back("USD stage authors time-sampled (animated) data: " +
            std::to_string(out.timeSampledTransformPrimCount) + " transform, " +
            std::to_string(out.timeSampledPointsPrimCount) + " deforming-points, and " +
            std::to_string(out.timeSampledVisibilityPrimCount) +
            " visibility prim(s); transform and camera/light parameter keyframes are decoded into runtime animation clips, while deforming mesh points are decoded during runtime mesh cook when topology is stable across samples.");
    }

    const size_t compositionArcPrimCount = out.referencePrimCount +
        out.payloadPrimCount + out.variantSetPrimCount + out.instancePrimCount;
    const bool stageHasComposition = out.sublayerCount > 0 || compositionArcPrimCount > 0;
    if (stageHasComposition) {
        out.warnings.push_back("USD stage relies on composition: " +
            std::to_string(out.sublayerCount) + " sublayer(s), " +
            std::to_string(out.referencePrimCount) + " referencing, " +
            std::to_string(out.payloadPrimCount) + " payload, " +
            std::to_string(out.variantSetPrimCount) + " variant-set, and " +
            std::to_string(out.instancePrimCount) +
            " instanced prim(s); OpenUSD composed these arcs, so traversal consumes the fully-composed result.");
    }

    out.diagnostics = {
        {"schema", "UsdStageImportDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", true},
        {"sourcePath", sourcePath.generic_string()},
        {"primCount", primCount},
        {"rootPrims", out.rootPrims},
        {"meshCount", out.meshCount},
        {"xformableCount", xformableCount},
        {"materialPrimCount", materialPrimCount},
        {"materialBindingCount", out.materialBindingCount},
        {"cameraCount", out.cameraCount},
        {"lightCount", out.lightCount},
        {"usdzPackage", isUsdz},
        {"metersPerUnit", out.metersPerUnit},
        {"metersPerUnitAuthored", metersPerUnitAuthored},
        {"upAxis", out.upAxis},
        {"upAxisConversionToYUpImplemented", true},
        {"upAxisConversionApplied", upAxisIsZ},
        {"metersPerUnitConversionImplemented", true},
        {"metersPerUnitConversionApplied", metersPerUnitNonMeter},
        {"stageMetricsImportImplemented", true},
        {"animation", {
            {"hasAuthoredTimeCodeRange", out.hasTimeSamples},
            {"startTimeCode", out.startTimeCode},
            {"endTimeCode", out.endTimeCode},
            {"timeCodesPerSecond", out.timeCodesPerSecond},
            {"timeSampledTransformPrimCount", out.timeSampledTransformPrimCount},
            {"timeSampledPointsPrimCount", out.timeSampledPointsPrimCount},
            {"timeSampledVisibilityPrimCount", out.timeSampledVisibilityPrimCount},
            {"decodedTransformAnimationChannelCount", out.decodedTransformAnimationChannelCount},
            {"decodedTransformAnimationKeyframeCount", out.decodedTransformAnimationKeyframeCount},
            {"decodedCameraLightAnimationChannelCount", out.decodedCameraLightAnimationChannelCount},
            {"decodedCameraLightAnimationKeyframeCount", out.decodedCameraLightAnimationKeyframeCount},
            {"animationClipCount", out.animations.size()},
            {"hasAnimation", stageHasAnimation},
            {"animationDetectionImplemented", true},
            {"runtimeTransformPlaybackImplemented", out.decodedTransformAnimationChannelCount > 0},
            {"runtimeMeshPointPlaybackImplemented", false},
            {"runtimeCameraLightParameterPlaybackImplemented", out.decodedCameraLightAnimationChannelCount > 0},
            {"runtimePlaybackImplemented", (out.decodedTransformAnimationChannelCount + out.decodedCameraLightAnimationChannelCount) > 0 && out.timeSampledPointsPrimCount == 0},
            {"policy", "time_sampled_transforms_and_camera_light_parameters_decoded_to_rtanim_mesh_point_deformation_decoded_during_runtime_mesh_cook"},
        }},
        {"composition", {
            {"sublayerCount", out.sublayerCount},
            {"referencePrimCount", out.referencePrimCount},
            {"payloadPrimCount", out.payloadPrimCount},
            {"variantSetPrimCount", out.variantSetPrimCount},
            {"variantSetCount", out.variantSetCount},
            {"instancePrimCount", out.instancePrimCount},
            {"prototypeCount", out.prototypeCount},
            {"instancedPrototypeMeshPrimCount", out.instancedPrototypeMeshPrimCount},
            {"instancesSharePrototypeGeometry", out.prototypeCount > 0},
            {"hasComposition", stageHasComposition},
            {"compositionDetectionImplemented", true},
            {"compositionResolvedByOpenUsd", true},
            {"policy", "composition_arcs_detected_and_reported_consumed_as_composed_stage"},
        }},
        {"primState", {
            {"invisiblePrimCount", out.invisiblePrimCount},
            {"inactivePrimCount", out.inactivePrimCount},
            {"guidePurposePrimCount", out.guidePurposePrimCount},
            {"proxyPurposePrimCount", out.proxyPurposePrimCount},
            {"renderPurposePrimCount", out.renderPurposePrimCount},
            {"visibilityPurposeActiveImportImplemented", true},
            {"runtimeCullingApplied", true},
            {"inactivePrimsPrunedByDefaultTraversal", true},
            {"policy", "visibility_purpose_active_detected_and_applied_to_runtime_mesh_and_light_visibility_inactive_prims_pruned_by_default_traversal"},
        }},
        {"meshNativeCookImplemented", false},
        {"materialBindingCookImplemented", false},
        {"materialNativeCookImplemented", false},
        {"shaderNetworkConversionImplemented", false},
        {"usdzTextureExtractionImplemented", false},
        {"parentHierarchyTransformCompositionImplemented", true},
        {"viewportPlacementImplemented", false},
        {"prims", out.prims},
        {"animations", out.animations},
        {"warnings", out.warnings},
        {"errors", out.errors},
    };
#else
    (void)sourcePath;
    out.supported = false;
    out.errors.push_back("USD stage import requires RTV_ENABLE_OPENUSD_IMPORTER=ON and OpenUSD availability.");
    out.diagnostics = {
        {"schema", "UsdStageImportDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", false},
        {"disabledReason", "RTV_ENABLE_OPENUSD_IMPORTER=OFF or OpenUSD unavailable"},
        {"sceneGraphMetadataImportImplemented", false},
        {"meshNativeCookImplemented", false},
        {"materialNativeCookImplemented", false},
        {"usdzTextureExtractionImplemented", false},
        {"viewportPlacementImplemented", false},
    };
#endif
    return out;
}

UsdRuntimeMeshCookData loadUsdRuntimeMeshes(const std::filesystem::path& sourcePath, std::string_view displayName) {
    UsdRuntimeMeshCookData out;
#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
    pxr::UsdStageRefPtr stage = openUsdStageForImport(sourcePath);
    if (!stage) {
        out.errors.push_back("OpenUSD failed to open the USD/USDZ stage for native mesh cooking.");
        out.diagnostics = {
            {"schema", "UsdRuntimeMeshCookDiagnosticsV1"},
            {"parser", "OpenUSD"},
            {"supported", false},
            {"sourcePath", sourcePath.generic_string()},
            {"errors", out.errors},
        };
        return out;
    }

    out.supported = true;
    const glm::mat4 stageConversion = usdStageConversionMatrix(
        pxr::UsdGeomGetStageMetersPerUnit(stage), pxr::UsdGeomGetStageUpAxis(stage).GetString());
    pxr::UsdGeomXformCache xformCache(pxr::UsdTimeCode::Default());
    nlohmann::json meshReports = nlohmann::json::array();
    size_t decodedMeshCount = 0;
    size_t uniqueDecodedMeshCount = 0;
    size_t sourcePrototypeGeometryReuseCount = 0;
    size_t decodedVertexCount = 0;
    size_t decodedIndexCount = 0;
    struct UsdDecodedPrototypeMesh {
        MeshAsset mesh;
        nlohmann::json report = nlohmann::json::object();
        std::string materialBindingPath;
        std::vector<std::string> primitiveMaterialBindingPaths;
    };
    std::unordered_map<std::string, UsdDecodedPrototypeMesh> decodedMeshByPrototypePrimPath;
    for (const pxr::UsdPrim& prim : usdStageImportPrimRange(stage)) {
        if (!prim || !prim.IsA<pxr::UsdGeomMesh>()) {
            continue;
        }
        ++out.meshPrimCount;
        std::string sourcePrototypeMeshPath;
        if (prim.IsInstanceProxy()) {
            const pxr::UsdPrim primInPrototype = prim.GetPrimInPrototype();
            if (primInPrototype) {
                sourcePrototypeMeshPath = primInPrototype.GetPath().GetString();
            }
        }
        nlohmann::json meshReport = nlohmann::json::object();
        std::optional<MeshAsset> decodedMesh;
        std::vector<std::string> primitiveMaterialPaths;
        const auto prototypeReuseIt = !sourcePrototypeMeshPath.empty()
            ? decodedMeshByPrototypePrimPath.find(sourcePrototypeMeshPath)
            : decodedMeshByPrototypePrimPath.end();
        if (prototypeReuseIt != decodedMeshByPrototypePrimPath.end()) {
            const UsdDecodedPrototypeMesh& shared = prototypeReuseIt->second;
            decodedMesh = shared.mesh;
            decodedMesh->name = prim.GetName().GetString();
            if (decodedMesh->name.empty()) {
                decodedMesh->name = shared.mesh.name;
            }
            meshReport = shared.report;
            meshReport["primPath"] = prim.GetPath().GetString();
            meshReport["name"] = decodedMesh->name;
            meshReport["transform"] = usdPrimTransformJson(prim, &xformCache, &stageConversion, false);
            meshReport["sourcePrototypeMeshPath"] = sourcePrototypeMeshPath;
            meshReport["sourcePrototypeGeometryReused"] = true;
            meshReport["sharedPrototypeDecodedMeshPath"] = shared.report.value("primPath", std::string{});
            const std::string materialBindingPath = usdMaterialBindingTargetPath(prim);
            meshReport["materialBindingPath"] = materialBindingPath.empty() ? shared.materialBindingPath : materialBindingPath;
            primitiveMaterialPaths = shared.primitiveMaterialBindingPaths;
            meshReport["primitiveMaterialBindingPaths"] = primitiveMaterialPaths;
            ++sourcePrototypeGeometryReuseCount;
        } else {
            decodedMesh = decodeUsdMeshAsset(pxr::UsdGeomMesh(prim), displayName, &xformCache, meshReport, out.warnings, &stageConversion);
            if (decodedMesh.has_value()) {
                ++uniqueDecodedMeshCount;
                if (!sourcePrototypeMeshPath.empty()) {
                    meshReport["sourcePrototypeMeshPath"] = sourcePrototypeMeshPath;
                    meshReport["sourcePrototypeGeometryReused"] = false;
                    std::vector<std::string> prototypePrimitiveMaterialPaths;
                    if (meshReport.contains("primitiveMaterialBindingPaths") && meshReport["primitiveMaterialBindingPaths"].is_array()) {
                        for (const nlohmann::json& value : meshReport["primitiveMaterialBindingPaths"]) {
                            if (value.is_string()) {
                                prototypePrimitiveMaterialPaths.push_back(value.get<std::string>());
                            }
                        }
                    }
                    decodedMeshByPrototypePrimPath.emplace(sourcePrototypeMeshPath, UsdDecodedPrototypeMesh{
                        .mesh = *decodedMesh,
                        .report = meshReport,
                        .materialBindingPath = meshReport.value("materialBindingPath", std::string{}),
                        .primitiveMaterialBindingPaths = std::move(prototypePrimitiveMaterialPaths),
                    });
                }
            }
        }
        if (!decodedMesh.has_value()) {
            ++out.skippedMeshPrimCount;
            meshReports.push_back(std::move(meshReport));
            continue;
        }
        decodedVertexCount += decodedMesh->vertices.size();
        decodedIndexCount += decodedMesh->indices.size();
        const std::string materialBindingPath = meshReport.value("materialBindingPath", std::string{});
        if (primitiveMaterialPaths.empty() && meshReport.contains("primitiveMaterialBindingPaths") && meshReport["primitiveMaterialBindingPaths"].is_array()) {
            for (const nlohmann::json& value : meshReport["primitiveMaterialBindingPaths"]) {
                if (value.is_string()) {
                    primitiveMaterialPaths.push_back(value.get<std::string>());
                }
            }
        }
        std::unordered_set<std::string> meshMaterialTargets;
        if (!materialBindingPath.empty()) {
            meshMaterialTargets.insert(materialBindingPath);
        }
        for (const std::string& primitiveMaterialPath : primitiveMaterialPaths) {
            if (!primitiveMaterialPath.empty()) {
                meshMaterialTargets.insert(primitiveMaterialPath);
            }
        }
        out.materialBindingTargetCount += meshMaterialTargets.size();
        ++decodedMeshCount;
        out.materialBindingPaths.push_back(materialBindingPath);
        out.primitiveMaterialBindingPaths.push_back(std::move(primitiveMaterialPaths));
        out.meshes.push_back(std::move(*decodedMesh));
        meshReports.push_back(std::move(meshReport));
    }

    if (out.meshPrimCount > 0 && out.meshes.empty()) {
        out.errors.push_back("OpenUSD found mesh prims, but no supported triangle topology could be decoded for native .rtmesh cooking.");
    }
    const bool isUsdz = lowerString(sourcePath.extension().string()) == ".usdz";
    if (isUsdz) {
        out.warnings.push_back("USDZ mesh topology can be decoded through OpenUSD; packaged texture extraction and native texture cooking run in the separate USD package texture cook stage.");
    }
    out.diagnostics = {
        {"schema", "UsdRuntimeMeshCookDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", true},
        {"sourcePath", sourcePath.generic_string()},
        {"meshPrimCount", out.meshPrimCount},
        {"decodedMeshCount", decodedMeshCount},
        {"uniqueDecodedMeshCount", uniqueDecodedMeshCount},
        {"sourcePrototypeGeometryReuseCount", sourcePrototypeGeometryReuseCount},
        {"skippedMeshPrimCount", out.skippedMeshPrimCount},
        {"materialBindingTargetCount", out.materialBindingTargetCount},
        {"decodedVertexCount", decodedVertexCount},
        {"decodedIndexCount", decodedIndexCount},
        {"triangulation", "deterministic_fan"},
        {"meshNativeCookImplemented", !out.meshes.empty()},
        {"materialBindingCookImplemented", out.materialBindingTargetCount > 0},
        {"materialNativeCookImplemented", out.materialBindingTargetCount > 0},
        {"shaderNetworkConversionImplemented", false},
        {"cameraLightRuntimeConversionImplemented", false},
        {"usdzTextureExtractionImplemented", false},
        {"parentHierarchyTransformCompositionImplemented", true},
        {"meshes", meshReports},
        {"warnings", out.warnings},
        {"errors", out.errors},
    };
#else
    (void)sourcePath;
    (void)displayName;
    out.supported = false;
    out.errors.push_back("USD runtime mesh cook requires RTV_ENABLE_OPENUSD_IMPORTER=ON and OpenUSD availability.");
    out.diagnostics = {
        {"schema", "UsdRuntimeMeshCookDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", false},
        {"disabledReason", "RTV_ENABLE_OPENUSD_IMPORTER=OFF or OpenUSD unavailable"},
        {"meshNativeCookImplemented", false},
        {"materialBindingCookImplemented", false},
        {"materialNativeCookImplemented", false},
        {"shaderNetworkConversionImplemented", false},
        {"cameraLightRuntimeConversionImplemented", false},
        {"usdzTextureExtractionImplemented", false},
    };
#endif
    return out;
}

UsdSceneEntityImportData usdSceneEntitiesFromStageMetadata(
    const std::filesystem::path& sourcePath,
    const UsdStageImportData& usdData) {
    UsdSceneEntityImportData out;
    out.supported = usdData.supported && usdData.errors.empty();
    out.cameras = usdData.cameras.is_array() ? usdData.cameras : nlohmann::json::array();
    out.lights = usdData.lights.is_array() ? usdData.lights : nlohmann::json::array();
    out.cameraCount = out.cameras.size();
    out.lightCount = out.lights.size();
    out.errors = usdData.errors;
    out.diagnostics = {
        {"schema", "UsdRuntimeSceneEntityDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", out.supported},
        {"sourcePath", sourcePath.generic_string()},
        {"cameraCount", out.cameraCount},
        {"lightCount", out.lightCount},
        {"cameraRuntimeConversionImplemented", out.cameraCount > 0},
        {"lightRuntimeConversionImplemented", out.lightCount > 0},
        {"parentHierarchyTransformCompositionImplemented", true},
        {"viewportPlacementImplemented", false},
        {"reusedStageMetadataTraversal", true},
        {"cameras", out.cameras},
        {"lights", out.lights},
        {"warnings", out.warnings},
        {"errors", out.errors},
    };
    return out;
}

UsdSceneEntityImportData loadUsdSceneEntities(const std::filesystem::path& sourcePath) {
    UsdSceneEntityImportData out;
#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
    pxr::UsdStageRefPtr stage = openUsdStageForImport(sourcePath);
    if (!stage) {
        out.errors.push_back("OpenUSD failed to open the USD/USDZ stage for camera/light conversion.");
        out.diagnostics = {
            {"schema", "UsdRuntimeSceneEntityDiagnosticsV1"},
            {"parser", "OpenUSD"},
            {"supported", false},
            {"sourcePath", sourcePath.generic_string()},
            {"errors", out.errors},
        };
        return out;
    }

    out.supported = true;
    const glm::mat4 stageConversion = usdStageConversionMatrix(
        pxr::UsdGeomGetStageMetersPerUnit(stage), pxr::UsdGeomGetStageUpAxis(stage).GetString());
    pxr::UsdGeomXformCache xformCache(pxr::UsdTimeCode::Default());
    for (const pxr::UsdPrim& prim : usdStageImportPrimRange(stage)) {
        if (!prim) {
            continue;
        }
        if (prim.IsA<pxr::UsdGeomCamera>()) {
            out.cameras.push_back(usdCameraRuntimeJson(pxr::UsdGeomCamera(prim), &xformCache, &stageConversion));
            ++out.cameraCount;
            continue;
        }
        if (usdPrimLooksLikeLight(prim)) {
            out.lights.push_back(usdLightRuntimeJson(prim, &xformCache, &stageConversion));
            ++out.lightCount;
        }
    }
    out.diagnostics = {
        {"schema", "UsdRuntimeSceneEntityDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", true},
        {"sourcePath", sourcePath.generic_string()},
        {"cameraCount", out.cameraCount},
        {"lightCount", out.lightCount},
        {"cameraRuntimeConversionImplemented", out.cameraCount > 0},
        {"lightRuntimeConversionImplemented", out.lightCount > 0},
        {"parentHierarchyTransformCompositionImplemented", true},
        {"viewportPlacementImplemented", false},
        {"cameras", out.cameras},
        {"lights", out.lights},
        {"warnings", out.warnings},
        {"errors", out.errors},
    };
#else
    (void)sourcePath;
    out.supported = false;
    out.errors.push_back("USD camera/light conversion requires RTV_ENABLE_OPENUSD_IMPORTER=ON and OpenUSD availability.");
    out.diagnostics = {
        {"schema", "UsdRuntimeSceneEntityDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", false},
        {"disabledReason", "RTV_ENABLE_OPENUSD_IMPORTER=OFF or OpenUSD unavailable"},
        {"cameraRuntimeConversionImplemented", false},
        {"lightRuntimeConversionImplemented", false},
        {"viewportPlacementImplemented", false},
    };
#endif
    return out;
}

void copyObjMaterialLibraries(
    const std::filesystem::path& originalSource,
    const std::filesystem::path& copiedSource,
    const nlohmann::json& objMetadata,
    const std::filesystem::path& allowedRoot,
    std::vector<std::filesystem::path>& generatedFiles,
    std::vector<std::string>& warnings,
    std::vector<std::string>& errors,
    std::unordered_set<std::string>& copiedKeys) {
    if (!objMetadata.contains("materialLibraries") || !objMetadata["materialLibraries"].is_array()) {
        return;
    }
    for (const nlohmann::json& item : objMetadata["materialLibraries"]) {
        if (!item.is_string()) {
            continue;
        }
        const std::string library = item.get<std::string>();
        const std::filesystem::path relativePath = std::filesystem::path(library).lexically_normal();
        if (relativePath.is_absolute()) {
            warnings.push_back("Skipped absolute OBJ material library while copying source assets: " + library);
            continue;
        }
        const std::filesystem::path sourcePath = (originalSource.parent_path() / relativePath).lexically_normal();
        const std::filesystem::path destinationPath = (copiedSource.parent_path() / relativePath).lexically_normal();
        if (!pathIsWithinLexicalRoot(destinationPath, allowedRoot)) {
            warnings.push_back("Skipped OBJ material library outside SourceAssets import folder: " + library);
            continue;
        }
        if (!copyImportSourceFile(sourcePath, destinationPath, generatedFiles, errors, copiedKeys)) {
            return;
        }
    }
}

uint32_t readLeU32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return static_cast<uint32_t>(bytes[offset])
        | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

nlohmann::json loadGltfJsonDocument(const std::filesystem::path& path, std::vector<std::string>& warnings) {
    const std::string ext = lowerString(path.extension().string());
    try {
        if (ext == ".gltf") {
            std::ifstream file(path);
            if (!file.is_open()) {
                warnings.push_back("Could not inspect glTF skeletal/animation metadata: source JSON could not be opened.");
                return {};
            }
            nlohmann::json json;
            file >> json;
            return json;
        }
        if (ext == ".glb") {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) {
                warnings.push_back("Could not inspect GLB skeletal/animation metadata: source file could not be opened.");
                return {};
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            constexpr uint32_t glbMagic = 0x46546C67u;
            constexpr uint32_t jsonChunkType = 0x4E4F534Au;
            if (bytes.size() < 20 || readLeU32(bytes, 0) != glbMagic || readLeU32(bytes, 4) != 2u) {
                warnings.push_back("Could not inspect GLB skeletal/animation metadata: unsupported GLB header.");
                return {};
            }
            const uint32_t jsonLength = readLeU32(bytes, 12);
            const uint32_t chunkType = readLeU32(bytes, 16);
            if (chunkType != jsonChunkType || 20ull + jsonLength > bytes.size()) {
                warnings.push_back("Could not inspect GLB skeletal/animation metadata: JSON chunk is missing or invalid.");
                return {};
            }
            const char* begin = reinterpret_cast<const char*>(bytes.data() + 20);
            return nlohmann::json::parse(begin, begin + jsonLength);
        }
    } catch (const std::exception& ex) {
        warnings.push_back(std::string("Could not inspect glTF skeletal/animation metadata: ") + ex.what());
    }
    return {};
}

std::string gltfNodeName(const nlohmann::json& doc, int nodeIndex) {
    if (doc.contains("nodes") && doc["nodes"].is_array() && nodeIndex >= 0 && static_cast<size_t>(nodeIndex) < doc["nodes"].size()) {
        return doc["nodes"][static_cast<size_t>(nodeIndex)].value("name", std::string{});
    }
    return {};
}

nlohmann::json gltfNodeRefMetadata(const nlohmann::json& doc, int nodeIndex) {
    nlohmann::json node = {
        {"index", nodeIndex},
        {"name", gltfNodeName(doc, nodeIndex)},
    };
    if (doc.contains("nodes") && doc["nodes"].is_array() && nodeIndex >= 0 && static_cast<size_t>(nodeIndex) < doc["nodes"].size()) {
        const nlohmann::json& sourceNode = doc["nodes"][static_cast<size_t>(nodeIndex)];
        if (sourceNode.contains("children") && sourceNode["children"].is_array()) {
            node["children"] = sourceNode["children"];
        }
    }
    return node;
}

std::vector<int> gltfDefaultSceneRootNodeIndices(const nlohmann::json& doc) {
    std::vector<int> roots;
    if (!doc.contains("scenes") || !doc["scenes"].is_array() || doc["scenes"].empty()) {
        return roots;
    }
    int sceneIndex = doc.value("scene", 0);
    if (sceneIndex < 0 || static_cast<size_t>(sceneIndex) >= doc["scenes"].size()) {
        sceneIndex = 0;
    }
    const nlohmann::json& scene = doc["scenes"][static_cast<size_t>(sceneIndex)];
    if (!scene.contains("nodes") || !scene["nodes"].is_array()) {
        return roots;
    }
    for (const nlohmann::json& node : scene["nodes"]) {
        if (node.is_number_integer()) {
            roots.push_back(node.get<int>());
        }
    }
    return roots;
}

nlohmann::json gltfSceneRootNodesMetadata(const nlohmann::json& doc, const std::vector<int>& rootNodes) {
    nlohmann::json roots = nlohmann::json::array();
    for (int nodeIndex : rootNodes) {
        roots.push_back(gltfNodeRefMetadata(doc, nodeIndex));
    }
    return roots;
}

std::unordered_map<int, std::string> gltfRootMotionNodeReasons(
    const nlohmann::json& doc,
    const std::vector<int>& sceneRootNodes) {
    std::unordered_map<int, std::string> reasons;
    for (int nodeIndex : sceneRootNodes) {
        reasons.emplace(nodeIndex, "scene_root");
    }
    if (doc.contains("skins") && doc["skins"].is_array()) {
        for (const nlohmann::json& skin : doc["skins"]) {
            if (skin.contains("skeleton") && skin["skeleton"].is_number_integer()) {
                const int skeletonRoot = skin["skeleton"].get<int>();
                auto [it, inserted] = reasons.emplace(skeletonRoot, "skeleton_root");
                if (!inserted && it->second.find("skeleton_root") == std::string::npos) {
                    it->second += "+skeleton_root";
                }
            }
        }
    }
    if (doc.contains("nodes") && doc["nodes"].is_array()) {
        for (size_t i = 0; i < doc["nodes"].size(); ++i) {
            const std::string lowerName = lowerString(doc["nodes"][i].value("name", std::string{}));
            if (lowerName.empty()) {
                continue;
            }
            const bool nameSuggestsRootMotion =
                lowerName == "root" ||
                lowerName == "hips" ||
                lowerName == "pelvis" ||
                lowerName.find("root") != std::string::npos ||
                lowerName.find("hip") != std::string::npos ||
                lowerName.find("pelvis") != std::string::npos ||
                lowerName.find("armature") != std::string::npos;
            if (!nameSuggestsRootMotion) {
                continue;
            }
            const int nodeIndex = static_cast<int>(i);
            auto [it, inserted] = reasons.emplace(nodeIndex, "name_hint");
            if (!inserted && it->second.find("name_hint") == std::string::npos) {
                it->second += "+name_hint";
            }
        }
    }
    return reasons;
}

nlohmann::json gltfAccessorMetadata(const nlohmann::json& doc, int accessorIndex) {
    nlohmann::json metadata = {
        {"index", accessorIndex},
        {"valid", false},
    };
    if (!doc.contains("accessors") || !doc["accessors"].is_array() || accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= doc["accessors"].size()) {
        return metadata;
    }
    const nlohmann::json& accessor = doc["accessors"][static_cast<size_t>(accessorIndex)];
    metadata["valid"] = true;
    metadata["count"] = accessor.value("count", 0u);
    metadata["type"] = accessor.value("type", std::string{});
    metadata["componentType"] = accessor.value("componentType", 0);
    metadata["bufferView"] = accessor.value("bufferView", -1);
    metadata["byteOffset"] = accessor.value("byteOffset", 0u);
    if (accessor.contains("min") && accessor["min"].is_array()) {
        metadata["min"] = accessor["min"];
    }
    if (accessor.contains("max") && accessor["max"].is_array()) {
        metadata["max"] = accessor["max"];
    }
    return metadata;
}

double gltfAccessorBoundValue(const nlohmann::json& doc, int accessorIndex, const char* key, double fallback) {
    if (!doc.contains("accessors") || !doc["accessors"].is_array() || accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= doc["accessors"].size()) {
        return fallback;
    }
    const nlohmann::json& accessor = doc["accessors"][static_cast<size_t>(accessorIndex)];
    if (!accessor.contains(key) || !accessor[key].is_array() || accessor[key].empty() || !accessor[key][0].is_number()) {
        return fallback;
    }
    return accessor[key][0].get<double>();
}

int gltfComponentSizeBytes(int componentType) {
    switch (componentType) {
    case kGltfComponentByte:
    case kGltfComponentUnsignedByte:
        return 1;
    case kGltfComponentShort:
    case kGltfComponentUnsignedShort:
        return 2;
    case 5125: // UNSIGNED_INT
    case kGltfComponentFloat:
        return 4;
    default:
        return 0;
    }
}

int gltfTypeComponentCount(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

float decodeGltfNumericComponent(const uint8_t* src, int componentType, bool normalized) {
    switch (componentType) {
    case kGltfComponentFloat: {
        float value = 0.0f;
        std::memcpy(&value, src, sizeof(value));
        return value;
    }
    case kGltfComponentByte: {
        int8_t value = 0;
        std::memcpy(&value, src, sizeof(value));
        return normalized ? std::max(-1.0f, static_cast<float>(value) / 127.0f) : static_cast<float>(value);
    }
    case kGltfComponentUnsignedByte: {
        uint8_t value = 0;
        std::memcpy(&value, src, sizeof(value));
        return normalized ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
    }
    case kGltfComponentShort: {
        int16_t value = 0;
        std::memcpy(&value, src, sizeof(value));
        return normalized ? std::max(-1.0f, static_cast<float>(value) / 32767.0f) : static_cast<float>(value);
    }
    case kGltfComponentUnsignedShort: {
        uint16_t value = 0;
        std::memcpy(&value, src, sizeof(value));
        return normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
    }
    case 5125: { // UNSIGNED_INT
        uint32_t value = 0;
        std::memcpy(&value, src, sizeof(value));
        return static_cast<float>(value);
    }
    default:
        return 0.0f;
    }
}

std::vector<uint8_t> readWholeFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> firstGlbBinChunk(const std::filesystem::path& path) {
    const std::vector<uint8_t> bytes = readWholeFileBytes(path);
    constexpr uint32_t glbMagic = 0x46546C67u;
    constexpr uint32_t jsonChunkType = 0x4E4F534Au;
    constexpr uint32_t binChunkType = 0x004E4942u;
    if (bytes.size() < 20 || readLeU32(bytes, 0) != glbMagic || readLeU32(bytes, 4) != 2u) {
        return {};
    }
    size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const uint32_t chunkLength = readLeU32(bytes, offset);
        const uint32_t chunkType = readLeU32(bytes, offset + 4);
        offset += 8;
        if (offset + chunkLength > bytes.size()) {
            return {};
        }
        if (chunkType == binChunkType) {
            return std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunkLength));
        }
        static_cast<void>(jsonChunkType);
        offset += chunkLength;
    }
    return {};
}

std::vector<std::vector<uint8_t>> loadGltfBufferPayloads(
    const std::filesystem::path& sourcePath,
    const nlohmann::json& doc,
    std::vector<std::string>& warnings) {
    std::vector<std::vector<uint8_t>> buffers;
    if (!doc.contains("buffers") || !doc["buffers"].is_array()) {
        return buffers;
    }
    buffers.resize(doc["buffers"].size());
    const bool sourceIsGlb = lowerString(sourcePath.extension().string()) == ".glb";
    std::vector<uint8_t> glbBin;
    for (size_t i = 0; i < doc["buffers"].size(); ++i) {
        const nlohmann::json& buffer = doc["buffers"][i];
        const std::string uri = buffer.value("uri", std::string{});
        if (uri.empty()) {
            if (sourceIsGlb) {
                if (glbBin.empty()) {
                    glbBin = firstGlbBinChunk(sourcePath);
                }
                buffers[i] = glbBin;
            }
            continue;
        }
        if (isDataUri(uri)) {
            std::optional<std::vector<uint8_t>> decoded = decodeDataUriPayload(uri);
            if (decoded.has_value()) {
                buffers[i] = std::move(*decoded);
            } else {
                warnings.push_back("Animation keyframe decode could not decode embedded data URI buffer " + std::to_string(i) + ".");
            }
            continue;
        }
        if (uri.find("://") != std::string::npos) {
            warnings.push_back("Animation keyframe decode skipped external glTF buffer URI " + std::to_string(i) + ": " + uri);
            continue;
        }
        const std::filesystem::path bufferPath = (sourcePath.parent_path() / decodeUriPath(uri)).lexically_normal();
        buffers[i] = readWholeFileBytes(bufferPath);
        if (buffers[i].empty()) {
            warnings.push_back("Animation keyframe decode could not read glTF buffer: " + bufferPath.string());
        }
    }
    return buffers;
}

struct DecodedGltfAccessor {
    size_t count = 0;
    int componentCount = 0;
    std::vector<float> values;
};

std::optional<DecodedGltfAccessor> decodeGltfAccessorFloats(
    const nlohmann::json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    int accessorIndex) {
    if (!doc.contains("accessors") || !doc["accessors"].is_array() || accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= doc["accessors"].size()) {
        return std::nullopt;
    }
    const nlohmann::json& accessor = doc["accessors"][static_cast<size_t>(accessorIndex)];
    const int bufferViewIndex = accessor.value("bufferView", -1);
    if (!doc.contains("bufferViews") || !doc["bufferViews"].is_array() || bufferViewIndex < 0 || static_cast<size_t>(bufferViewIndex) >= doc["bufferViews"].size()) {
        return std::nullopt;
    }
    const nlohmann::json& view = doc["bufferViews"][static_cast<size_t>(bufferViewIndex)];
    const int bufferIndex = view.value("buffer", -1);
    if (bufferIndex < 0 || static_cast<size_t>(bufferIndex) >= buffers.size() || buffers[static_cast<size_t>(bufferIndex)].empty()) {
        return std::nullopt;
    }
    const int componentType = accessor.value("componentType", 0);
    const int componentSize = gltfComponentSizeBytes(componentType);
    const int componentCount = gltfTypeComponentCount(accessor.value("type", std::string{}));
    if (componentSize <= 0 || componentCount <= 0) {
        return std::nullopt;
    }
    const size_t accessorCount = accessor.value("count", 0u);
    const size_t elementSize = static_cast<size_t>(componentSize) * static_cast<size_t>(componentCount);
    const size_t stride = view.contains("byteStride") && view["byteStride"].is_number_integer()
        ? static_cast<size_t>(std::max(0, view["byteStride"].get<int>()))
        : elementSize;
    if (stride < elementSize) {
        return std::nullopt;
    }
    const size_t viewOffset = view.value("byteOffset", 0u);
    const size_t accessorOffset = accessor.value("byteOffset", 0u);
    const size_t viewLength = view.value("byteLength", 0u);
    const std::vector<uint8_t>& buffer = buffers[static_cast<size_t>(bufferIndex)];
    if (viewOffset > buffer.size() || viewLength > buffer.size() - viewOffset || accessorOffset > viewLength) {
        return std::nullopt;
    }
    if (accessorCount > 0) {
        const size_t lastElementOffset = accessorOffset + (accessorCount - 1) * stride;
        if (lastElementOffset > viewLength || elementSize > viewLength - lastElementOffset) {
            return std::nullopt;
        }
    }
    DecodedGltfAccessor decoded;
    decoded.count = accessorCount;
    decoded.componentCount = componentCount;
    decoded.values.resize(accessorCount * static_cast<size_t>(componentCount));
    const bool normalized = accessor.value("normalized", false);
    const uint8_t* base = buffer.data() + viewOffset + accessorOffset;
    for (size_t i = 0; i < accessorCount; ++i) {
        const uint8_t* element = base + i * stride;
        for (int c = 0; c < componentCount; ++c) {
            decoded.values[i * static_cast<size_t>(componentCount) + static_cast<size_t>(c)] =
                decodeGltfNumericComponent(element + static_cast<size_t>(c) * static_cast<size_t>(componentSize), componentType, normalized);
        }
    }
    return decoded;
}

nlohmann::json decodedAnimationTrackJson(
    const nlohmann::json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const nlohmann::json& animation,
    const nlohmann::json& channel) {
    nlohmann::json result = {
        {"decoded", false},
        {"runtimeSupport", "decoded_keyframes_runtime_playback_supported"},
    };
    if (!channel.is_object() || !channel.contains("target") || !channel["target"].is_object() ||
        !channel.contains("sampler") || !channel["sampler"].is_number_integer() ||
        !animation.contains("samplers") || !animation["samplers"].is_array()) {
        result["reason"] = "missing_channel_or_sampler";
        return result;
    }
    const int samplerIndex = channel["sampler"].get<int>();
    if (samplerIndex < 0 || static_cast<size_t>(samplerIndex) >= animation["samplers"].size()) {
        result["reason"] = "sampler_out_of_range";
        return result;
    }
    const nlohmann::json& sampler = animation["samplers"][static_cast<size_t>(samplerIndex)];
    const std::string path = channel["target"].value("path", std::string{});
    const std::string interpolation = sampler.value("interpolation", std::string("LINEAR"));
    const bool cubicSpline = interpolation == "CUBICSPLINE";
    const auto input = decodeGltfAccessorFloats(doc, buffers, sampler.value("input", -1));
    const auto output = decodeGltfAccessorFloats(doc, buffers, sampler.value("output", -1));
    if (!input.has_value() || !output.has_value() || input->componentCount != 1 || input->count == 0) {
        result["reason"] = "input_or_output_accessor_decode_failed";
        return result;
    }
    const size_t keyCount = input->count;
    nlohmann::json times = nlohmann::json::array();
    for (size_t i = 0; i < keyCount; ++i) {
        times.push_back(input->values[i]);
    }
    nlohmann::json values = nlohmann::json::array();
    nlohmann::json inTangents = nlohmann::json::array();
    nlohmann::json outTangents = nlohmann::json::array();
    if (path == "translation" || path == "scale" || path == "rotation") {
        const int expectedComponents = path == "rotation" ? 4 : 3;
        if (output->componentCount != expectedComponents) {
            result["reason"] = "unexpected_output_component_count";
            result["outputComponentCount"] = output->componentCount;
            return result;
        }
        const size_t requiredElements = keyCount * (cubicSpline ? 3u : 1u);
        if (output->count < requiredElements) {
            result["reason"] = "output_accessor_too_short";
            return result;
        }
        auto vectorAtElement = [&](size_t elementIndex) {
            nlohmann::json value = nlohmann::json::array();
            for (int c = 0; c < expectedComponents; ++c) {
                value.push_back(output->values[elementIndex * static_cast<size_t>(expectedComponents) + static_cast<size_t>(c)]);
            }
            return value;
        };
        for (size_t i = 0; i < keyCount; ++i) {
            const size_t elementIndex = cubicSpline ? (i * 3u + 1u) : i;
            if (cubicSpline) {
                inTangents.push_back(vectorAtElement(i * 3u));
            }
            values.push_back(vectorAtElement(elementIndex));
            if (cubicSpline) {
                outTangents.push_back(vectorAtElement(i * 3u + 2u));
            }
        }
    } else if (path == "weights") {
        size_t valuesPerKey = 0;
        if (output->componentCount == 1) {
            const size_t divisor = keyCount * (cubicSpline ? 3u : 1u);
            valuesPerKey = divisor > 0 ? output->count / divisor : 0;
        } else {
            valuesPerKey = static_cast<size_t>(output->componentCount);
        }
        if (valuesPerKey == 0) {
            result["reason"] = "could_not_determine_weight_count";
            return result;
        }
        const size_t requiredScalars = keyCount * valuesPerKey * (cubicSpline ? 3u : 1u);
        if (output->values.size() < requiredScalars) {
            result["reason"] = "output_weight_accessor_too_short";
            return result;
        }
        auto weightsAtOffset = [&](size_t scalarOffset) {
            nlohmann::json value = nlohmann::json::array();
            for (size_t c = 0; c < valuesPerKey; ++c) {
                value.push_back(output->values[scalarOffset + c]);
            }
            return value;
        };
        for (size_t i = 0; i < keyCount; ++i) {
            const size_t scalarOffset = (cubicSpline ? (i * 3u + 1u) : i) * valuesPerKey;
            if (cubicSpline) {
                inTangents.push_back(weightsAtOffset(i * 3u * valuesPerKey));
            }
            values.push_back(weightsAtOffset(scalarOffset));
            if (cubicSpline) {
                outTangents.push_back(weightsAtOffset((i * 3u + 2u) * valuesPerKey));
            }
        }
        result["weightCount"] = valuesPerKey;
    } else {
        result["reason"] = "unsupported_target_path";
        return result;
    }
    result["decoded"] = true;
    result["targetPath"] = path;
    result["interpolation"] = interpolation;
    result["keyframeCount"] = keyCount;
    result["times"] = times;
    result["values"] = values;
    if (cubicSpline) {
        result["inTangents"] = inTangents;
        result["outTangents"] = outTangents;
    }
    return result;
}

std::string animationTargetPathLabel(const nlohmann::json& channel, const nlohmann::json& doc) {
    if (!channel.is_object() || !channel.contains("target") || !channel["target"].is_object()) {
        return "unknown";
    }
    const nlohmann::json& target = channel["target"];
    std::string path = target.value("path", std::string("unknown"));
    if (target.contains("node") && target["node"].is_number_integer()) {
        const int nodeIndex = target["node"].get<int>();
        const std::string nodeName = gltfNodeName(doc, nodeIndex);
        if (!nodeName.empty()) {
            path += "@" + nodeName;
        }
    }
    return path;
}

nlohmann::json gltfSkeletalAnimationMetadata(const std::filesystem::path& sourcePath, std::vector<std::string>& warnings) {
    nlohmann::json metadata = {
        {"inspected", false},
        {"sourceFormat", lowerString(sourcePath.extension().string())},
        {"skinCount", 0},
        {"animationCount", 0},
        {"skins", nlohmann::json::array()},
        {"animations", nlohmann::json::array()},
        {"runtimeSupport", "not_cooked_for_runtime_playback"},
    };

    const nlohmann::json doc = loadGltfJsonDocument(sourcePath, warnings);
    if (!doc.is_object()) {
        return metadata;
    }
    metadata["inspected"] = true;
    const std::vector<std::vector<uint8_t>> animationBuffers = loadGltfBufferPayloads(sourcePath, doc, warnings);
    const std::vector<int> sceneRootNodes = gltfDefaultSceneRootNodeIndices(doc);
    const std::unordered_map<int, std::string> rootMotionNodeReasons = gltfRootMotionNodeReasons(doc, sceneRootNodes);
    metadata["sceneRootNodes"] = gltfSceneRootNodesMetadata(doc, sceneRootNodes);

    if (doc.contains("skins") && doc["skins"].is_array()) {
        metadata["skinCount"] = doc["skins"].size();
        for (size_t i = 0; i < doc["skins"].size(); ++i) {
            const nlohmann::json& skin = doc["skins"][i];
            nlohmann::json joints = nlohmann::json::array();
            nlohmann::json jointHierarchy = nlohmann::json::array();
            if (skin.contains("joints") && skin["joints"].is_array()) {
                for (const nlohmann::json& joint : skin["joints"]) {
                    if (!joint.is_number_integer()) {
                        continue;
                    }
                    const int jointNode = joint.get<int>();
                    joints.push_back(gltfNodeRefMetadata(doc, jointNode));
                    nlohmann::json hierarchyNode = gltfNodeRefMetadata(doc, jointNode);
                    hierarchyNode["role"] = "joint";
                    jointHierarchy.push_back(std::move(hierarchyNode));
                }
            }
            const int skeletonRoot = skin.contains("skeleton") && skin["skeleton"].is_number_integer() ? skin["skeleton"].get<int>() : -1;
            const int inverseBindAccessor = skin.contains("inverseBindMatrices") && skin["inverseBindMatrices"].is_number_integer()
                ? skin["inverseBindMatrices"].get<int>()
                : -1;
            metadata["skins"].push_back({
                {"index", i},
                {"name", skin.value("name", std::string{})},
                {"jointCount", joints.size()},
                {"joints", joints},
                {"jointHierarchy", jointHierarchy},
                {"skeletonRoot", skeletonRoot >= 0 ? gltfNodeRefMetadata(doc, skeletonRoot) : nlohmann::json::object()},
                {"hasSkeletonRoot", skeletonRoot >= 0},
                {"inverseBindMatricesAccessor", inverseBindAccessor},
                {"inverseBindMatrices", gltfAccessorMetadata(doc, inverseBindAccessor)},
                {"hasInverseBindMatrices", inverseBindAccessor >= 0},
                {"runtimeSupport", "cpu_current_pose_skinning_supported"},
            });
        }
    }

    if (doc.contains("animations") && doc["animations"].is_array()) {
        metadata["animationCount"] = doc["animations"].size();
        for (size_t i = 0; i < doc["animations"].size(); ++i) {
            const nlohmann::json& animation = doc["animations"][i];
            nlohmann::json targetPaths = nlohmann::json::array();
            nlohmann::json samplers = nlohmann::json::array();
            nlohmann::json channels = nlohmann::json::array();
            nlohmann::json tracks = nlohmann::json::array();
            nlohmann::json trackLookup = nlohmann::json::object();
            nlohmann::json rootMotionCandidates = nlohmann::json::array();
            std::unordered_set<std::string> uniqueTargetPaths;
            size_t decodedChannelCount = 0;
            size_t decodedKeyframeCount = 0;
            double clipStart = std::numeric_limits<double>::max();
            double clipEnd = 0.0;
            if (animation.contains("samplers") && animation["samplers"].is_array()) {
                for (size_t samplerIndex = 0; samplerIndex < animation["samplers"].size(); ++samplerIndex) {
                    const nlohmann::json& sampler = animation["samplers"][samplerIndex];
                    const int inputAccessor = sampler.value("input", -1);
                    const int outputAccessor = sampler.value("output", -1);
                    const double samplerStart = gltfAccessorBoundValue(doc, inputAccessor, "min", 0.0);
                    const double samplerEnd = gltfAccessorBoundValue(doc, inputAccessor, "max", 0.0);
                    if (inputAccessor >= 0) {
                        clipStart = std::min(clipStart, samplerStart);
                        clipEnd = std::max(clipEnd, samplerEnd);
                    }
                    samplers.push_back({
                        {"index", samplerIndex},
                        {"inputAccessor", inputAccessor},
                        {"outputAccessor", outputAccessor},
                        {"input", gltfAccessorMetadata(doc, inputAccessor)},
                        {"output", gltfAccessorMetadata(doc, outputAccessor)},
                        {"interpolation", sampler.value("interpolation", std::string("LINEAR"))},
                        {"timeStart", samplerStart},
                        {"timeEnd", samplerEnd},
                    });
                }
            }
            if (animation.contains("channels") && animation["channels"].is_array()) {
                for (size_t channelIndex = 0; channelIndex < animation["channels"].size(); ++channelIndex) {
                    const nlohmann::json& channel = animation["channels"][channelIndex];
                    const std::string path = animationTargetPathLabel(channel, doc);
                    if (uniqueTargetPaths.insert(path).second) {
                        targetPaths.push_back(path);
                    }
                    const nlohmann::json target = channel.contains("target") && channel["target"].is_object()
                        ? channel["target"]
                        : nlohmann::json::object();
                    const int targetNode = target.value("node", -1);
                    nlohmann::json decodedTrack = decodedAnimationTrackJson(doc, animationBuffers, animation, channel);
                    if (decodedTrack.value("decoded", false)) {
                        ++decodedChannelCount;
                        decodedKeyframeCount += decodedTrack.value("keyframeCount", 0u);
                    }
                    const std::string targetPath = target.value("path", std::string("unknown"));
                    const std::string lookupKey = std::to_string(targetNode) + ":" + targetPath;
                    const nlohmann::json trackSummary = {
                        {"trackIndex", tracks.size()},
                        {"channelIndex", channelIndex},
                        {"sampler", channel.value("sampler", -1)},
                        {"target", {
                            {"node", targetNode},
                            {"nodeName", gltfNodeName(doc, targetNode)},
                            {"path", targetPath},
                            {"label", path},
                        }},
                        {"decoded", decodedTrack.value("decoded", false)},
                        {"keyframeCount", decodedTrack.value("keyframeCount", 0u)},
                        {"interpolation", decodedTrack.value("interpolation", std::string{})},
                        {"hasCubicSplineTangents", decodedTrack.contains("inTangents") && decodedTrack.contains("outTangents")},
                        {"lookupKey", lookupKey},
                    };
                    const auto rootMotionIt = rootMotionNodeReasons.find(targetNode);
                    const bool rootMotionPath = targetPath == "translation" || targetPath == "rotation";
                    if (decodedTrack.value("decoded", false) && rootMotionPath && rootMotionIt != rootMotionNodeReasons.end()) {
                        rootMotionCandidates.push_back({
                            {"trackIndex", trackSummary["trackIndex"]},
                            {"channelIndex", channelIndex},
                            {"sampler", channel.value("sampler", -1)},
                            {"node", targetNode},
                            {"nodeName", gltfNodeName(doc, targetNode)},
                            {"path", targetPath},
                            {"label", path},
                            {"candidateReason", rootMotionIt->second},
                            {"keyframeCount", decodedTrack.value("keyframeCount", 0u)},
                            {"interpolation", decodedTrack.value("interpolation", std::string{})},
                            {"hasCubicSplineTangents", decodedTrack.contains("inTangents") && decodedTrack.contains("outTangents")},
                            {"runtimeSupport", "root_motion_candidate_metadata_available_runtime_playback_supported"},
                        });
                    }
                    tracks.push_back(trackSummary);
                    if (!trackLookup.contains(lookupKey)) {
                        trackLookup[lookupKey] = nlohmann::json::array();
                    }
                    trackLookup[lookupKey].push_back(trackSummary["trackIndex"]);
                    channels.push_back({
                        {"index", channelIndex},
                        {"sampler", channel.value("sampler", -1)},
                        {"target", {
                            {"node", targetNode},
                            {"nodeName", gltfNodeName(doc, targetNode)},
                            {"path", targetPath},
                            {"label", path},
                        }},
                        {"decodedTrack", decodedTrack},
                    });
                }
            }
            if (clipStart == std::numeric_limits<double>::max()) {
                clipStart = 0.0;
            }
            metadata["animations"].push_back({
                {"index", i},
                {"name", animation.value("name", std::string{})},
                {"channelCount", animation.contains("channels") && animation["channels"].is_array() ? animation["channels"].size() : 0},
                {"samplerCount", animation.contains("samplers") && animation["samplers"].is_array() ? animation["samplers"].size() : 0},
                {"decodedChannelCount", decodedChannelCount},
                {"decodedKeyframeCount", decodedKeyframeCount},
                {"samplers", samplers},
                {"channels", channels},
                {"tracks", tracks},
                {"trackLookup", trackLookup},
                {"rootMotionCandidates", rootMotionCandidates},
                {"rootMotionCandidateCount", rootMotionCandidates.size()},
                {"rootMotionSupport", "root_motion_candidate_metadata_available_runtime_playback_supported"},
                {"targetPaths", targetPaths},
                {"clip", {
                    {"startTime", clipStart},
                    {"endTime", clipEnd},
                    {"duration", std::max(0.0, clipEnd - clipStart)},
                    {"runtimeSupport", "decoded_keyframes_runtime_playback_supported"},
                }},
            });
        }
    }

    const size_t skinCount = metadata.value("skinCount", 0u);
    const size_t animationCount = metadata.value("animationCount", 0u);
    if (skinCount > 0 || animationCount > 0) {
        warnings.push_back(
            "Source contains skeletal or animation data; skin payloads and decoded animation tracks feed the shared runtime playback and GPU skinning path when generated native assets are present.");
    }
    return metadata;
}

nlohmann::json gltfCollisionLodMetadata(const std::filesystem::path& sourcePath, std::vector<std::string>& warnings) {
    nlohmann::json collisionCandidates = nlohmann::json::array();
    nlohmann::json lodCandidates = nlohmann::json::array();
    std::unordered_set<std::string> uniqueCollisionCandidates;
    std::unordered_set<std::string> uniqueLodCandidates;

    const nlohmann::json doc = loadGltfJsonDocument(sourcePath, warnings);
    if (!doc.is_object()) {
        return collisionLodMetadataJson("glTF", false, collisionCandidates, lodCandidates);
    }
    if (doc.contains("meshes") && doc["meshes"].is_array()) {
        for (size_t i = 0; i < doc["meshes"].size(); ++i) {
            const nlohmann::json& mesh = doc["meshes"][i];
            appendCollisionLodNameMetadata(
                mesh.value("name", std::string{}),
                "mesh",
                i,
                collisionCandidates,
                lodCandidates,
                uniqueCollisionCandidates,
                uniqueLodCandidates);
        }
    }
    if (doc.contains("nodes") && doc["nodes"].is_array()) {
        for (size_t i = 0; i < doc["nodes"].size(); ++i) {
            const nlohmann::json& node = doc["nodes"][i];
            appendCollisionLodNameMetadata(
                node.value("name", std::string{}),
                "node",
                i,
                collisionCandidates,
                lodCandidates,
                uniqueCollisionCandidates,
                uniqueLodCandidates);
        }
    }

    if (!collisionCandidates.empty()) {
        addWarningOnce(warnings, "Source contains collision-named glTF meshes/nodes; collision metadata was preserved for downstream physics/runtime tooling.");
    }
    if (!lodCandidates.empty()) {
        addWarningOnce(warnings, "Source contains LOD-named glTF meshes/nodes; LOD metadata was preserved for downstream renderer/runtime tooling.");
    }
    return collisionLodMetadataJson("glTF", true, collisionCandidates, lodCandidates);
}

nlohmann::json importerCapabilityReport(
    const std::filesystem::path& sourcePath,
    AssetType type,
    const AssetImportSettings& settings,
    const nlohmann::json& objMetadata,
    const nlohmann::json& mtlMetadata,
    const nlohmann::json& usdMetadata,
    const nlohmann::json& skeletalAnimationMetadata,
    const nlohmann::json& collisionLodMetadata,
    std::vector<std::string>& warnings) {
#if !(RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE)
    (void)usdMetadata;
#endif
    const std::string ext = lowerString(sourcePath.extension().string());
    nlohmann::json report = {
        {"version", 1},
        {"sourceFormat", ext.empty() ? std::string("unknown") : ext},
        {"assetType", assetTypeName(type)},
        {"features", nlohmann::json::array()},
        {"metadataOnlyFeatures", nlohmann::json::array()},
        {"unsupportedFeatures", nlohmann::json::array()},
        {"disabledBySettings", nlohmann::json::array()},
    };

    auto addFeature = [&](std::string name, std::string support, nlohmann::json detail = nlohmann::json::object()) {
        nlohmann::json feature = {
            {"name", std::move(name)},
            {"support", std::move(support)},
            {"detail", std::move(detail)},
        };
        const std::string supportValue = feature.value("support", std::string{});
        report["features"].push_back(feature);
        if (supportValue == "metadata_only" || supportValue == "pending_runtime_cook") {
            report["metadataOnlyFeatures"].push_back(feature);
        } else if (supportValue == "unsupported" || supportValue == "blocking_unsupported_required") {
            report["unsupportedFeatures"].push_back(feature);
        } else if (supportValue == "disabled_by_import_settings") {
            report["disabledBySettings"].push_back(feature);
        }
    };

    auto addCollisionLodFeatures = [&]() {
        if (!collisionLodMetadata.is_object()) {
            return;
        }
        const size_t collisionCandidateCount = collisionLodMetadata.value("collisionCandidateCount", 0u);
        const size_t lodCandidateCount = collisionLodMetadata.value("lodCandidateCount", 0u);
        if (collisionCandidateCount > 0) {
            addFeature("collisionMetadata", "supported", {
                {"candidateCount", collisionCandidateCount},
                {"candidates", collisionLodMetadata.value("collisionCandidates", nlohmann::json::array())},
                {"runtimeSupport", "authored_collision_metadata_preserved"},
                {"runtimeSystemScope", "physics_collision_backend_not_part_of_asset_importer"},
            });
        }
        if (lodCandidateCount > 0) {
            addFeature("lodMetadata", "supported", {
                {"candidateCount", lodCandidateCount},
                {"candidates", collisionLodMetadata.value("lodCandidates", nlohmann::json::array())},
                {"runtimeSupport", "authored_lod_metadata_preserved"},
                {"runtimeSystemScope", "renderer_lod_selection_not_part_of_asset_importer"},
            });
        }
    };

    if (ext == ".gltf" || ext == ".glb") {
        const nlohmann::json doc = loadGltfJsonDocument(sourcePath, warnings);
        if (!doc.is_object()) {
            addFeature("sourceInspection", "unsupported", {{"reason", "glTF JSON could not be inspected"}});
            return report;
        }

        const size_t meshCount = doc.contains("meshes") && doc["meshes"].is_array() ? doc["meshes"].size() : 0;
        const size_t materialCount = doc.contains("materials") && doc["materials"].is_array() ? doc["materials"].size() : 0;
        const size_t textureCount = doc.contains("textures") && doc["textures"].is_array() ? doc["textures"].size() : 0;
        size_t basisTextureCount = 0;
        size_t invalidBasisTextureSourceCount = 0;
        if (doc.contains("textures") && doc["textures"].is_array()) {
            const size_t imageCount = doc.contains("images") && doc["images"].is_array() ? doc["images"].size() : 0;
            for (const nlohmann::json& texture : doc["textures"]) {
                if (!texture.contains("extensions") || !texture["extensions"].is_object() ||
                    !texture["extensions"].contains("KHR_texture_basisu") ||
                    !texture["extensions"]["KHR_texture_basisu"].is_object()) {
                    continue;
                }
                const nlohmann::json& basisExt = texture["extensions"]["KHR_texture_basisu"];
                if (!basisExt.contains("source") || !basisExt["source"].is_number_integer()) {
                    ++invalidBasisTextureSourceCount;
                    continue;
                }
                const int source = basisExt["source"].get<int>();
                if (source < 0 || static_cast<size_t>(source) >= imageCount) {
                    ++invalidBasisTextureSourceCount;
                    continue;
                }
                ++basisTextureCount;
            }
        }
        const size_t cameraCount = doc.contains("cameras") && doc["cameras"].is_array() ? doc["cameras"].size() : 0;
        size_t materialVariantCount = 0;
        size_t materialVariantMappingCount = 0;
        size_t lightCount = 0;
        if (doc.contains("extensions") && doc["extensions"].is_object() &&
            doc["extensions"].contains("KHR_materials_variants") &&
            doc["extensions"]["KHR_materials_variants"].is_object()) {
            const nlohmann::json& variantsExt = doc["extensions"]["KHR_materials_variants"];
            if (variantsExt.contains("variants") && variantsExt["variants"].is_array()) {
                materialVariantCount = variantsExt["variants"].size();
            }
        }
        if (doc.contains("extensions") && doc["extensions"].is_object()) {
            const nlohmann::json& extensions = doc["extensions"];
            if (extensions.contains("KHR_lights_punctual") && extensions["KHR_lights_punctual"].contains("lights") &&
                extensions["KHR_lights_punctual"]["lights"].is_array()) {
                lightCount = extensions["KHR_lights_punctual"]["lights"].size();
            }
        }

        size_t morphTargetPrimitiveCount = 0;
        size_t renderablePrimitiveCount = 0;
        size_t unsupportedPrimitiveModeCount = 0;
        nlohmann::json primitiveModeCounts = nlohmann::json::object();
        nlohmann::json unsupportedPrimitiveModes = nlohmann::json::array();
        size_t morphTargetCount = 0;
        size_t morphPositionTargetCount = 0;
        size_t morphNormalTargetCount = 0;
        size_t morphTangentTargetCount = 0;
        size_t vertexColorPrimitiveCount = 0;
        size_t secondUvPrimitiveCount = 0;
        size_t extraUvPrimitiveCount = 0;
        size_t textureTexCoord1SlotCount = 0;
        size_t unsupportedTextureTexCoordSlotCount = 0;
        uint32_t highestVertexTexCoordSet = 0;
        uint32_t highestTextureTexCoordSet = 0;
        std::unordered_set<std::string> extraUvAttributeNames;
        nlohmann::json unsupportedTextureTexCoordSlots = nlohmann::json::array();
        size_t skinnedPrimitiveCount = 0;
        size_t jointAttributePrimitiveCount = 0;
        size_t weightAttributePrimitiveCount = 0;
        size_t extraJointWeightSetPrimitiveCount = 0;
        size_t quantizedFloatAttributeCount = 0;
        size_t unsupportedQuantizedFloatAttributeCount = 0;
        std::unordered_set<int> quantizedFloatAccessors;
        std::vector<std::string> quantizedFloatAttributeNames;
        std::unordered_set<std::string> quantizedFloatAttributeNameSet;
        auto inspectQuantizedFloatAttribute = [&](const nlohmann::json& attributes,
                                                  const char* attributeName,
                                                  GltfQuantizedFloatPolicy policy) {
            if (!attributes.contains(attributeName) || !attributes[attributeName].is_number_integer()) {
                return;
            }
            const int accessorIndex = attributes[attributeName].get<int>();
            const nlohmann::json* accessor = gltfAccessorJson(doc, accessorIndex);
            if (accessor == nullptr) {
                return;
            }
            const int componentType = accessor->value("componentType", 0);
            if (componentType == kGltfComponentFloat) {
                return;
            }
            if (!gltfQuantizedFloatComponentType(componentType)) {
                return;
            }
            const bool normalized = accessor->value("normalized", false);
            if (!gltfQuantizedFloatComponentAllowed(componentType, normalized, policy)) {
                ++unsupportedQuantizedFloatAttributeCount;
                return;
            }
            ++quantizedFloatAttributeCount;
            quantizedFloatAccessors.insert(accessorIndex);
            if (quantizedFloatAttributeNameSet.insert(attributeName).second) {
                quantizedFloatAttributeNames.push_back(attributeName);
            }
        };
        auto inspectTexCoordAttributeName = [&](const std::string& attributeName) {
            constexpr std::string_view prefix = "TEXCOORD_";
            if (attributeName.rfind(prefix, 0) != 0) {
                return;
            }
            const std::string suffix = attributeName.substr(prefix.size());
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(suffix.c_str(), &end, 10);
            if (end == suffix.c_str() || *end != '\0') {
                return;
            }
            highestVertexTexCoordSet = std::max(highestVertexTexCoordSet, static_cast<uint32_t>(parsed));
            if (parsed > 1ul) {
                extraUvAttributeNames.insert(attributeName);
            }
        };
        auto inspectTextureInfoTexCoord = [&](size_t materialIndex,
                                              const std::string& materialName,
                                              std::string_view slot,
                                              const nlohmann::json& textureInfo) {
            if (!textureInfo.is_object()) {
                return;
            }
            int texCoord = textureInfo.value("texCoord", 0);
            std::string texCoordSource = "textureInfo";
            if (textureInfo.contains("extensions") && textureInfo["extensions"].is_object() &&
                textureInfo["extensions"].contains("KHR_texture_transform") &&
                textureInfo["extensions"]["KHR_texture_transform"].is_object()) {
                const nlohmann::json& transform = textureInfo["extensions"]["KHR_texture_transform"];
                if (transform.contains("texCoord") && transform["texCoord"].is_number_integer()) {
                    texCoord = transform["texCoord"].get<int>();
                    texCoordSource = "KHR_texture_transform";
                }
            }
            if (texCoord < 0) {
                return;
            }
            highestTextureTexCoordSet = std::max(highestTextureTexCoordSet, static_cast<uint32_t>(texCoord));
            if (texCoord == 1) {
                ++textureTexCoord1SlotCount;
            } else if (texCoord > 1) {
                ++unsupportedTextureTexCoordSlotCount;
                unsupportedTextureTexCoordSlots.push_back({
                    {"materialIndex", materialIndex},
                    {"materialName", materialName},
                    {"slot", std::string(slot)},
                    {"texCoord", texCoord},
                    {"source", texCoordSource},
                    {"runtimeFallback", "TEXCOORD_0"},
                    {"reason", "runtime mesh vertex format stores TEXCOORD_0 and TEXCOORD_1 only"},
                });
            }
        };
        auto inspectTextureInfoMember = [&](size_t materialIndex,
                                            const std::string& materialName,
                                            const nlohmann::json& object,
                                            std::string_view slot) {
            const std::string key(slot);
            if (object.is_object() && object.contains(key)) {
                inspectTextureInfoTexCoord(materialIndex, materialName, slot, object[key]);
            }
        };
        if (doc.contains("materials") && doc["materials"].is_array()) {
            for (size_t materialIndex = 0; materialIndex < doc["materials"].size(); ++materialIndex) {
                const nlohmann::json& material = doc["materials"][materialIndex];
                if (!material.is_object()) {
                    continue;
                }
                const std::string materialName = material.value("name", std::string{});
                if (material.contains("pbrMetallicRoughness") && material["pbrMetallicRoughness"].is_object()) {
                    const nlohmann::json& pbr = material["pbrMetallicRoughness"];
                    inspectTextureInfoMember(materialIndex, materialName, pbr, "baseColorTexture");
                    inspectTextureInfoMember(materialIndex, materialName, pbr, "metallicRoughnessTexture");
                }
                inspectTextureInfoMember(materialIndex, materialName, material, "normalTexture");
                inspectTextureInfoMember(materialIndex, materialName, material, "occlusionTexture");
                inspectTextureInfoMember(materialIndex, materialName, material, "emissiveTexture");
                if (material.contains("extensions") && material["extensions"].is_object()) {
                    const nlohmann::json& extensions = material["extensions"];
                    auto inspectExtensionTexture = [&](const char* extensionName, std::initializer_list<std::string_view> slots) {
                        if (!extensions.contains(extensionName) || !extensions[extensionName].is_object()) {
                            return;
                        }
                        const nlohmann::json& extension = extensions[extensionName];
                        for (std::string_view slot : slots) {
                            inspectTextureInfoMember(materialIndex, materialName, extension, slot);
                        }
                    };
                    inspectExtensionTexture("KHR_materials_pbrSpecularGlossiness", {"diffuseTexture", "specularGlossinessTexture"});
                    inspectExtensionTexture("KHR_materials_clearcoat", {"clearcoatTexture", "clearcoatRoughnessTexture", "clearcoatNormalTexture"});
                    inspectExtensionTexture("KHR_materials_transmission", {"transmissionTexture"});
                    inspectExtensionTexture("KHR_materials_volume", {"thicknessTexture"});
                    inspectExtensionTexture("KHR_materials_specular", {"specularTexture", "specularColorTexture"});
                    inspectExtensionTexture("KHR_materials_sheen", {"sheenColorTexture", "sheenRoughnessTexture"});
                    inspectExtensionTexture("KHR_materials_iridescence", {"iridescenceTexture", "iridescenceThicknessTexture"});
                    inspectExtensionTexture("KHR_materials_anisotropy", {"anisotropyTexture"});
                }
            }
        }
        if (doc.contains("meshes") && doc["meshes"].is_array()) {
            for (const nlohmann::json& mesh : doc["meshes"]) {
                if (!mesh.contains("primitives") || !mesh["primitives"].is_array()) {
                    continue;
                }
                for (const nlohmann::json& primitive : mesh["primitives"]) {
                    const int primitiveMode = primitive.value("mode", 4);
                    const std::string primitiveModeName = gltfPrimitiveModeLabel(primitiveMode);
                    primitiveModeCounts[primitiveModeName] = primitiveModeCounts.value(primitiveModeName, 0u) + 1u;
                    if (gltfPrimitiveModeRenderableAsTriangles(primitiveMode)) {
                        ++renderablePrimitiveCount;
                    } else {
                        ++unsupportedPrimitiveModeCount;
                        unsupportedPrimitiveModes.push_back({
                            {"meshName", mesh.value("name", std::string{})},
                            {"mode", primitiveMode},
                            {"modeName", primitiveModeName},
                            {"runtimeFallback", "primitive_skipped"},
                            {"reason", "runtime mesh path supports triangles, triangle strips, and triangle fans only"},
                        });
                    }
                    if (primitive.contains("attributes") && primitive["attributes"].is_object()) {
                        const nlohmann::json& attributes = primitive["attributes"];
                        for (auto it = attributes.begin(); it != attributes.end(); ++it) {
                            inspectTexCoordAttributeName(it.key());
                        }
                        inspectQuantizedFloatAttribute(attributes, "POSITION", GltfQuantizedFloatPolicy::PositionOrTexcoord);
                        inspectQuantizedFloatAttribute(attributes, "NORMAL", GltfQuantizedFloatPolicy::SignedNormalizedInteger);
                        inspectQuantizedFloatAttribute(attributes, "TANGENT", GltfQuantizedFloatPolicy::SignedNormalizedInteger);
                        inspectQuantizedFloatAttribute(attributes, "TEXCOORD_0", GltfQuantizedFloatPolicy::PositionOrTexcoord);
                        inspectQuantizedFloatAttribute(attributes, "TEXCOORD_1", GltfQuantizedFloatPolicy::PositionOrTexcoord);
                        if (attributes.contains("COLOR_0")) {
                            ++vertexColorPrimitiveCount;
                        }
                        if (attributes.contains("TEXCOORD_1")) {
                            ++secondUvPrimitiveCount;
                        }
                        if (std::any_of(extraUvAttributeNames.begin(), extraUvAttributeNames.end(), [&](const std::string& name) {
                                return attributes.contains(name);
                            })) {
                            ++extraUvPrimitiveCount;
                        }
                        const bool hasJoints = attributes.contains("JOINTS_0");
                        const bool hasWeights = attributes.contains("WEIGHTS_0");
                        const bool hasExtraJoints = attributes.contains("JOINTS_1");
                        const bool hasExtraWeights = attributes.contains("WEIGHTS_1");
                        if (hasJoints) {
                            ++jointAttributePrimitiveCount;
                        }
                        if (hasWeights) {
                            ++weightAttributePrimitiveCount;
                        }
                        if (hasJoints && hasWeights) {
                            ++skinnedPrimitiveCount;
                        }
                        if (hasExtraJoints && hasExtraWeights) {
                            ++extraJointWeightSetPrimitiveCount;
                        }
                    }
                    if (primitive.contains("extensions") && primitive["extensions"].is_object() &&
                        primitive["extensions"].contains("KHR_materials_variants") &&
                        primitive["extensions"]["KHR_materials_variants"].is_object()) {
                        const nlohmann::json& variantsExt = primitive["extensions"]["KHR_materials_variants"];
                        if (variantsExt.contains("mappings") && variantsExt["mappings"].is_array()) {
                            materialVariantMappingCount += variantsExt["mappings"].size();
                        }
                    }
                    if (primitive.contains("targets") && primitive["targets"].is_array() && !primitive["targets"].empty()) {
                        ++morphTargetPrimitiveCount;
                        morphTargetCount += primitive["targets"].size();
                        for (const nlohmann::json& target : primitive["targets"]) {
                            if (!target.is_object()) {
                                continue;
                            }
                            if (target.contains("POSITION")) {
                                ++morphPositionTargetCount;
                                inspectQuantizedFloatAttribute(target, "POSITION", GltfQuantizedFloatPolicy::SignedPositionDelta);
                            }
                            if (target.contains("NORMAL")) {
                                ++morphNormalTargetCount;
                                inspectQuantizedFloatAttribute(target, "NORMAL", GltfQuantizedFloatPolicy::SignedNormalizedInteger);
                            }
                            if (target.contains("TANGENT")) {
                                ++morphTangentTargetCount;
                                inspectQuantizedFloatAttribute(target, "TANGENT", GltfQuantizedFloatPolicy::SignedNormalizedInteger);
                            }
                        }
                    }
                }
            }
        }

        addFeature("meshes", "supported", {{"count", meshCount}});
        if (!primitiveModeCounts.empty()) {
            addFeature("primitiveModes", unsupportedPrimitiveModeCount > 0 ? "unsupported" : "supported", {
                {"modeCounts", primitiveModeCounts},
                {"renderablePrimitiveCount", renderablePrimitiveCount},
                {"unsupportedPrimitiveModeCount", unsupportedPrimitiveModeCount},
                {"unsupportedPrimitiveModes", unsupportedPrimitiveModes},
                {"runtimeSupport", unsupportedPrimitiveModeCount > 0
                    ? "non_triangle_point_line_modes_reported_and_skipped"
                    : "triangles_triangle_strips_triangle_fans_renderable"},
            });
            if (unsupportedPrimitiveModeCount > 0) {
                addWarningOnce(warnings, "Source contains glTF point or line primitive modes; runtime mesh import currently supports triangles, triangle strips, and triangle fans, so those primitives are skipped.");
            }
        }
        addFeature("materials", settings.importMaterials
            ? (settings.materialImportMode == "MetadataOnly" ? "metadata_only" : "supported")
            : "disabled_by_import_settings", {
                {"count", materialCount},
                {"materialImportMode", settings.materialImportMode},
            });
        addFeature("textures", settings.importTextures
            ? (settings.textureImportMode == "MetadataOnly" ? "metadata_only" : "supported")
            : "disabled_by_import_settings", {
                {"count", textureCount},
                {"textureImportMode", settings.textureImportMode},
                {"textureCompression", settings.textureCompression},
            });
        const bool basisTextureDeclared =
            jsonStringArrayContains(doc, "extensionsUsed", "KHR_texture_basisu") ||
            jsonStringArrayContains(doc, "extensionsRequired", "KHR_texture_basisu");
        if (basisTextureDeclared || basisTextureCount > 0 || invalidBasisTextureSourceCount > 0) {
            addFeature("basisUniversalTextures", settings.importTextures ? "supported" : "disabled_by_import_settings", {
                {"extension", "KHR_texture_basisu"},
                {"declared", basisTextureDeclared},
                {"textureCount", basisTextureCount},
                {"invalidTextureSourceCount", invalidBasisTextureSourceCount},
                {"container", "KTX2"},
                {"runtimeSupport", "KHR_texture_basisu_source_images_loaded_through_KTX2_BasisU_transcode_path"},
            });
            if (invalidBasisTextureSourceCount > 0) {
                addWarningOnce(warnings, "Source contains KHR_texture_basisu textures with invalid source references; those textures fall back to the core glTF texture source when available.");
            }
        }
        addFeature("cameras", settings.importCameras ? "supported" : "disabled_by_import_settings", {{"count", cameraCount}});
        addFeature("punctualLights", settings.importLights ? "supported" : "disabled_by_import_settings", {{"count", lightCount}});
        if (vertexColorPrimitiveCount > 0) {
            addFeature("vertexColors", "supported", {{"primitiveCount", vertexColorPrimitiveCount}, {"attribute", "COLOR_0"}});
        }
        if (secondUvPrimitiveCount > 0) {
            addFeature("secondUvSet", "supported", {
                {"primitiveCount", secondUvPrimitiveCount},
                {"attribute", "TEXCOORD_1"},
            });
        }
        if (highestVertexTexCoordSet > 1 || highestTextureTexCoordSet > 1 || textureTexCoord1SlotCount > 0) {
            nlohmann::json extraUvAttributes = nlohmann::json::array();
            for (const std::string& name : extraUvAttributeNames) {
                extraUvAttributes.push_back(name);
            }
            addFeature("textureCoordinateSets",
                (unsupportedTextureTexCoordSlotCount > 0 || extraUvPrimitiveCount > 0) ? "unsupported" : "supported", {
                    {"runtimeStoredSets", nlohmann::json::array({"TEXCOORD_0", "TEXCOORD_1"})},
                    {"highestVertexTexCoordSet", highestVertexTexCoordSet},
                    {"highestTextureTexCoordSet", highestTextureTexCoordSet},
                    {"textureTexCoord1SlotCount", textureTexCoord1SlotCount},
                    {"unsupportedTextureTexCoordSlotCount", unsupportedTextureTexCoordSlotCount},
                    {"unsupportedTextureTexCoordSlots", unsupportedTextureTexCoordSlots},
                    {"extraVertexTexCoordPrimitiveCount", extraUvPrimitiveCount},
                    {"extraVertexTexCoordAttributes", extraUvAttributes},
                    {"runtimeSupport", unsupportedTextureTexCoordSlotCount > 0 || extraUvPrimitiveCount > 0
                        ? "TEXCOORD_2_plus_reported_with_TEXCOORD_0_runtime_fallback"
                        : "TEXCOORD_0_and_TEXCOORD_1_runtime_sampling_supported"},
                });
            if (unsupportedTextureTexCoordSlotCount > 0 || extraUvPrimitiveCount > 0) {
                addWarningOnce(warnings, "Source uses glTF TEXCOORD_2 or higher; runtime mesh/material sampling currently supports TEXCOORD_0 and TEXCOORD_1, so affected texture slots fall back to TEXCOORD_0.");
            }
        }
        const bool meshQuantizationDeclared =
            jsonStringArrayContains(doc, "extensionsUsed", "KHR_mesh_quantization") ||
            jsonStringArrayContains(doc, "extensionsRequired", "KHR_mesh_quantization");
        if (meshQuantizationDeclared || quantizedFloatAttributeCount > 0 || unsupportedQuantizedFloatAttributeCount > 0) {
            addFeature("meshQuantization", "supported", {
                {"extension", "KHR_mesh_quantization"},
                {"declared", meshQuantizationDeclared},
                {"decodedAttributeCount", quantizedFloatAttributeCount},
                {"decodedAccessorCount", quantizedFloatAccessors.size()},
                {"decodedAttributes", quantizedFloatAttributeNames},
                {"unsupportedQuantizedAttributeCount", unsupportedQuantizedFloatAttributeCount},
                {"runtimeSupport", "integer_position_texcoord_and_signed_normal_tangent_attributes_decoded_to_runtime_float_vertices"},
                {"cachePolicy", "scene_cache_rebuild_required_for_previous_partial_imports"},
            });
            if (unsupportedQuantizedFloatAttributeCount > 0) {
                addWarningOnce(warnings, "Source contains quantized integer mesh attributes outside the supported KHR_mesh_quantization attribute/component combinations; those attributes may be skipped by runtime import.");
            }
        }
        if (jointAttributePrimitiveCount > 0 || weightAttributePrimitiveCount > 0) {
            addFeature("skinningVertexPayload", "supported", {
                {"skinnedPrimitiveCount", skinnedPrimitiveCount},
                {"jointAttributePrimitiveCount", jointAttributePrimitiveCount},
                {"weightAttributePrimitiveCount", weightAttributePrimitiveCount},
                {"extraJointWeightSetPrimitiveCount", extraJointWeightSetPrimitiveCount},
                {"attributes", extraJointWeightSetPrimitiveCount > 0
                    ? nlohmann::json::array({"JOINTS_0", "WEIGHTS_0", "JOINTS_1", "WEIGHTS_1"})
                    : nlohmann::json::array({"JOINTS_0", "WEIGHTS_0"})},
                {"runtimeSupport", extraJointWeightSetPrimitiveCount > 0
                    ? "shared_runtime_gpu_skinning_supported_with_extra_influences_compacted_to_top4"
                    : "shared_runtime_gpu_skinning_supported"},
                {"animationPlayback", "decoded_keyframes_runtime_playback_supported_when_animation_channels_exist"},
            });
        }
        if (materialVariantCount > 0 || materialVariantMappingCount > 0) {
            addFeature("materialVariants", "supported", {
                {"variantCount", materialVariantCount},
                {"mappingCount", materialVariantMappingCount},
                {"extension", "KHR_materials_variants"},
            });
        }

        const size_t skinCount = skeletalAnimationMetadata.value("skinCount", 0u);
        const size_t animationCount = skeletalAnimationMetadata.value("animationCount", 0u);
        size_t decodedAnimationChannelCount = 0;
        size_t decodedAnimationKeyframeCount = 0;
        size_t animationTrackCount = 0;
        size_t rootMotionCandidateCount = 0;
        if (skeletalAnimationMetadata.contains("animations") && skeletalAnimationMetadata["animations"].is_array()) {
            for (const nlohmann::json& animation : skeletalAnimationMetadata["animations"]) {
                decodedAnimationChannelCount += animation.value("decodedChannelCount", 0u);
                decodedAnimationKeyframeCount += animation.value("decodedKeyframeCount", 0u);
                animationTrackCount += animation.contains("tracks") && animation["tracks"].is_array() ? animation["tracks"].size() : 0;
                rootMotionCandidateCount += animation.value("rootMotionCandidateCount", 0u);
            }
        }
        if (skinCount > 0) {
            addFeature("skins", "supported", {
                {"count", skinCount},
                {"metadataAsset", ".rtskeleton.json"},
                {"preservedFields", nlohmann::json::array({"joints", "jointHierarchy", "skeletonRoot", "inverseBindMatrices"})},
                {"runtimeSupport", "shared_runtime_gpu_skinning_supported"},
            });
        }
        if (animationCount > 0) {
            addFeature("animations", "metadata_only", {
                {"count", animationCount},
                {"metadataAsset", ".rtanim.json"},
                {"decodedChannelCount", decodedAnimationChannelCount},
                {"decodedKeyframeCount", decodedAnimationKeyframeCount},
                {"trackCount", animationTrackCount},
                {"rootMotionCandidateCount", rootMotionCandidateCount},
                {"preservedFields", nlohmann::json::array({"samplers", "channels", "tracks", "trackLookup", "rootMotionCandidates", "interpolation", "targetNodes", "clipTiming", "decodedTimes", "decodedValues", "decodedTangents"})},
                {"runtimeSupport", "decoded_keyframes_runtime_playback_supported"},
            });
        }
        if (morphTargetCount > 0) {
            addFeature("morphTargets", "supported", {
                {"targetCount", morphTargetCount},
                {"primitiveCount", morphTargetPrimitiveCount},
                {"positionDeltaTargetCount", morphPositionTargetCount},
                {"normalDeltaTargetCount", morphNormalTargetCount},
                {"tangentDeltaTargetCount", morphTangentTargetCount},
                {"preservedFields", nlohmann::json::array({"POSITION", "NORMAL", "TANGENT", "targetNames"})},
                {"runtimeSupport", "shared_runtime_morph_weight_playback_supported"},
            });
        }
        addCollisionLodFeatures();

        report["extensionsUsed"] = jsonStringArray(doc, "extensionsUsed");
        report["extensionsRequired"] = jsonStringArray(doc, "extensionsRequired");
        nlohmann::json unsupportedExtensions = nlohmann::json::array();
        bool hasUnsupportedRequiredExtension = false;
        auto inspectExtensionList = [&](const nlohmann::json& extensions, bool required) {
            for (const nlohmann::json& item : extensions) {
                if (!item.is_string()) {
                    continue;
                }
                const std::string extension = item.get<std::string>();
                if (!supportedGltfExtensionForImportReport(extension)) {
                    unsupportedExtensions.push_back({
                        {"name", extension},
                        {"required", required},
                        {"severity", required ? "blocking" : "warning"},
                        {"runtimeSupport", required ? "blocked_until_extension_decoder_is_implemented" : "ignored_optional_extension"},
                    });
                    hasUnsupportedRequiredExtension = hasUnsupportedRequiredExtension || required;
                    addWarningOnce(warnings, std::string("glTF import report: unsupported ") + (required ? "required " : "") + "extension '" + extension + "'.");
                }
            }
        };
        inspectExtensionList(report["extensionsUsed"], false);
        inspectExtensionList(report["extensionsRequired"], true);
        if (!unsupportedExtensions.empty()) {
            addFeature("extensions", hasUnsupportedRequiredExtension ? "blocking_unsupported_required" : "unsupported", {
                {"unsupported", unsupportedExtensions},
                {"blocksRuntimeImport", hasUnsupportedRequiredExtension},
            });
        } else if (!report["extensionsUsed"].empty() || !report["extensionsRequired"].empty()) {
            addFeature("extensions", "supported", {{"used", report["extensionsUsed"]}, {"required", report["extensionsRequired"]}});
        }
    } else if (ext == ".obj") {
        addFeature("objGeometry", "supported", {
            {"vertexCount", objMetadata.value("vertexCount", 0u)},
            {"faceCount", objMetadata.value("faceCount", 0u)},
            {"runtimeSupport", "native_rtmesh_cook_supported_with_uvs_vertex_colors_generated_normals_and_generated_tangents"},
        });
        addFeature("objMaterialLibraries", "supported", {
            {"materialLibraries", objMetadata.value("materialLibraries", nlohmann::json::array())},
            {"runtimeSupport", "mtl_rtmaterial_rttexture_cook_and_material_slot_guid_binding_supported"},
        });
        addCollisionLodFeatures();
    } else if (ext == ".mtl") {
        addFeature("mtlMaterials", "supported", {
            {"materialCount", mtlMetadata.value("materialCount", 0u)},
            {"textureMapCount", mtlMetadata.value("textureMapCount", 0u)},
            {"runtimeSupport", "native_rtmaterial_cook_supported"},
        });
        addFeature("mtlTextureReferences", "supported", {
            {"textureReferences", mtlMetadata.value("textureReferences", nlohmann::json::array())},
            {"runtimeSupport", "native_rttexture_cook_and_material_slot_guid_binding_supported_where_slots_exist"},
        });
    } else if (ext == ".fbx") {
#if RTV_ENABLE_ASSIMP_IMPORTER && RTV_ASSIMP_IMPORTER_AVAILABLE
        const size_t fbxSkeletonCount = skeletalAnimationMetadata.contains("skeletons") && skeletalAnimationMetadata["skeletons"].is_array()
            ? skeletalAnimationMetadata["skeletons"].size()
            : skeletalAnimationMetadata.value("skeletonCount", 0u);
        const size_t fbxAnimationCount = skeletalAnimationMetadata.contains("animations") && skeletalAnimationMetadata["animations"].is_array()
            ? skeletalAnimationMetadata["animations"].size()
            : skeletalAnimationMetadata.value("animationCount", 0u);
        addFeature("fbxStaticMeshes", "supported", {
            {"parser", "assimp"},
            {"runtimeSupport", "native_rtmesh_and_rtmaterial_cook_supported_for_static_fbx_scene"},
            {"textureSupport", "external_and_embedded_texture_references_cooked_to_native_rttexture"},
            {"skeletalSupport", fbxSkeletonCount > 0 ? "metadata_bridge" : "not_present"},
            {"animationSupport", fbxAnimationCount > 0 ? "metadata_bridge" : "not_present"},
        });
        addFeature("fbxMaterials", settings.importMaterials ? "supported" : "disabled_by_import_settings", {
            {"parser", "assimp"},
            {"conversion", "diffuse/emissive/specular/shininess/opacity_to_native_pbr_approximation"},
            {"textureBindings", "native_texture_guid_slots_for_external_and_embedded_texture_references"},
        });
        addFeature("fbxHierarchy", "supported", {
            {"runtimeSupport", "prefab_metadata_hierarchy_with_mesh_guid_bindings"},
            {"multiMeshNodePolicy", "first_mesh_only_in_foundation_slice"},
        });
        addFeature("fbxTextures", settings.importTextures ? "supported" : "disabled_by_import_settings", {
            {"runtimeSupport", "external_and_embedded_texture_references_decode_and_cook_to_native_rttexture"},
            {"embeddedTextureSupport", "supported"},
            {"supportedSlots", nlohmann::json::array({"baseColor", "emissive", "normal", "metallicRoughness", "occlusion", "specular"})},
        });
        if (fbxSkeletonCount > 0 || fbxAnimationCount > 0) {
            addFeature("fbxSkeletalAnimation", "binding_decode_bridge", {
                {"skeletonCount", fbxSkeletonCount},
                {"animationCount", fbxAnimationCount},
                {"skeletonMetadataAsset", ".rtskeleton.json"},
                {"skeletalMeshBindingAsset", ".rtskeletalmesh.json"},
                {"animationMetadataAsset", ".rtanim.json"},
                {"animationControllerAsset", ".rtanimcontroller.json"},
                {"nativePayloads", nlohmann::json::array({".rtskeleton", ".rtanim", ".rtanimcontroller", ".rtskeletalmesh"})},
                {"preservedFields", nlohmann::json::array({"jointHierarchy", "inverseBindMatrices", "boneWeights", "animationChannels", "positionRotationScaleKeys"})},
                {"runtimeSupport", "decoded_keyframes_with_default_controller_binding_supported_when_animation_channels_exist"},
            });
        } else {
            addFeature("fbxSkeletalAnimation", "supported", {
                {"skeletonCount", 0},
                {"animationCount", 0},
                {"runtimeSupport", "no_skeletal_animation_payload_detected"},
            });
        }
#else
        addFeature("fbxStaticMeshes", "unsupported", {
            {"reason", "RTV_ENABLE_ASSIMP_IMPORTER=OFF or Assimp unavailable"},
        });
#endif
    } else if (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz") {
#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
        const size_t primCount = usdMetadata.value("primCount", 0u);
        const size_t meshCount = usdMetadata.value("meshCount", 0u);
        const size_t nativeMeshCount = usdMetadata.value("meshNativeCookAssetCount", 0u);
        const bool meshNativeCookImplemented = usdMetadata.value("meshNativeCookImplemented", false);
        const size_t materialBindingCount = usdMetadata.value("materialBindingCount", 0u);
        const size_t nativeMaterialCount = usdMetadata.value("materialNativeCookAssetCount", 0u);
        const bool materialNativeCookImplemented = usdMetadata.value("materialNativeCookImplemented", false);
        const bool textureNativeCookImplemented = usdMetadata.value("textureNativeCookImplemented", false);
        const bool usdzTextureNativeCookImplemented = usdMetadata.value("usdzTextureNativeCookImplemented", false);
        const bool usdExternalTextureNativeCookImplemented = usdMetadata.value("usdExternalTextureNativeCookImplemented", false);
        const size_t usdzNativeTextureCookCount = usdMetadata.value("usdzNativeTextureCookCount", 0u);
        const size_t usdExternalTextureCookCount = usdMetadata.value("usdExternalTextureCookCount", 0u);
        const size_t runtimeCameraCount = usdMetadata.value("runtimeCameraCount", 0u);
        const size_t runtimeLightCount = usdMetadata.value("runtimeLightCount", 0u);
        const bool cameraRuntimeConversionImplemented = usdMetadata.value("cameraRuntimeConversionImplemented", false);
        const bool lightRuntimeConversionImplemented = usdMetadata.value("lightRuntimeConversionImplemented", false);
        const bool runtimePackagePlacementImplemented = usdMetadata.value("runtimePackagePlacementImplemented", false);
        const bool runtimeReloadParityImplemented = usdMetadata.value("runtimeReloadParityImplemented", false);
        addFeature("usdStageGraph", meshNativeCookImplemented ? "supported_with_native_mesh_payloads" : "metadata_only", {
            {"parser", "OpenUSD"},
            {"primCount", primCount},
            {"rootPrims", usdMetadata.value("rootPrims", nlohmann::json::array())},
            {"preservedFields", nlohmann::json::array({"primPath", "parentPath", "typeName", "localTransform", "meshAttributes", "materialBindingFlag", "cameraFlag", "lightFlag"})},
            {"runtimeSupport", meshNativeCookImplemented ? "stage_metadata_plus_native_rtmesh_payloads" : "metadata_bridge_only_native_usd_scene_cook_pending"},
            {"runtimePackagePlacementImplemented", runtimePackagePlacementImplemented},
            {"runtimeReloadParityImplemented", runtimeReloadParityImplemented},
        });
        addFeature("usdMeshes", meshNativeCookImplemented || meshCount == 0 ? "supported" : "pending_runtime_cook", {
            {"meshCount", meshCount},
            {"nativeMeshCount", nativeMeshCount},
            {"nativePayload", ".rtmesh"},
            {"runtimeSupport", meshNativeCookImplemented ? "usd_mesh_topology_decoded_to_native_rtmesh" : meshCount > 0 ? "usd_mesh_native_cook_pending" : "no_mesh_prims_detected"},
        });
        addFeature("usdMaterialBindings", materialNativeCookImplemented || materialBindingCount == 0 ? "supported" : "pending_runtime_cook", {
            {"materialBindingCount", materialBindingCount},
            {"nativeMaterialCount", nativeMaterialCount},
            {"nativePayloads", nlohmann::json::array({".rtmaterial", ".rttexture"})},
            {"runtimeSupport", materialNativeCookImplemented ? "usd_material_binding_rtmaterial_payloads_with_texture_guid_bindings_when_references_resolve" : materialBindingCount > 0 ? "usd_material_native_cook_pending" : "no_material_bindings_detected"},
            {"shaderNetworkConversionImplemented", usdMetadata.value("shaderNetworkConversionImplemented", false)},
            {"textureNativeCookImplemented", textureNativeCookImplemented},
            {"usdExternalTextureNativeCookImplemented", usdExternalTextureNativeCookImplemented},
            {"usdExternalTextureCookCount", usdExternalTextureCookCount},
            {"usdzTextureNativeCookImplemented", usdzTextureNativeCookImplemented},
            {"usdzNativeTextureCookCount", usdzNativeTextureCookCount},
        });
        addFeature("usdSceneEntities", (cameraRuntimeConversionImplemented || lightRuntimeConversionImplemented) ? "supported_with_runtime_entities" : "metadata_only", {
            {"cameraCount", usdMetadata.value("cameraCount", 0u)},
            {"lightCount", usdMetadata.value("lightCount", 0u)},
            {"runtimeCameraCount", runtimeCameraCount},
            {"runtimeLightCount", runtimeLightCount},
            {"runtimeSupport", (cameraRuntimeConversionImplemented || lightRuntimeConversionImplemented) ? "usd_camera_light_runtime_conversion_supported" : "no_runtime_camera_or_light_entities_detected"},
            {"cameraRuntimeConversionImplemented", cameraRuntimeConversionImplemented},
            {"lightRuntimeConversionImplemented", lightRuntimeConversionImplemented},
            {"viewportPlacementImplemented", runtimePackagePlacementImplemented},
            {"runtimePackagePlacementImplemented", runtimePackagePlacementImplemented},
            {"runtimeReloadParityImplemented", runtimeReloadParityImplemented},
        });
        if (ext == ".usdz") {
            addFeature("usdzPackageTextures", usdzTextureNativeCookImplemented ? "supported" : "pending_runtime_cook", {
                {"runtimeSupport", usdzTextureNativeCookImplemented ? "usdz_texture_extraction_and_native_rttexture_cook_supported" : "usdz_texture_extraction_and_native_rttexture_cook_pending"},
                {"usdzTextureNativeCookImplemented", usdzTextureNativeCookImplemented},
                {"usdzNativeTextureCookCount", usdzNativeTextureCookCount},
            });
        }
#else
        addFeature("usdStageGraph", "unsupported", {
            {"reason", "RTV_ENABLE_OPENUSD_IMPORTER=OFF or OpenUSD unavailable"},
        });
#endif
    } else if (type == AssetType::Texture || type == AssetType::HDRI) {
        const nlohmann::json textureRole = inferTextureRole(sourcePath, type);
        if (ext == ".basis") {
            addFeature("basisStandalone", "unsupported", {
                {"reason", "Standalone .basis files are unsupported; wrap BasisU payloads in KTX2 via KHR_texture_basisu."},
                {"runtimePayload", "unsupported_basis_standalone"},
            });
            addWarningOnce(warnings, "Standalone .basis texture import is unsupported; use KTX2/KHR_texture_basisu instead.");
        } else {
            addFeature(type == AssetType::HDRI ? "environmentImage" : "textureImage", "supported", {
                {"runtimePayload", "native_rttexture_cook_supported"},
                {"rendererUpload", "TextureLoader_createTexture2D_uses_source_or_compressed_vk_format_and_mip_table"},
            });
        }
        addFeature("textureRoleDetection", "supported", textureRole);
        if (ext == ".dds" || ext == ".ktx" || ext == ".ktx2") {
            addFeature("compressedTexturePolicy", "supported", {
                {"policy", ext == ".ktx2" ? "preserve_or_transcode_ktx2_according_to_native_texture_policy" : "preserve_supported_dds_native_payloads"},
                {"nativeTextureCookImplemented", true},
                {"rendererUploadImplemented", true},
                {"standaloneBasisSupported", false},
            });
        }
    } else {
        addFeature("sourceFormat", "unsupported", {{"reason", "no source importer is registered for this extension"}});
    }

    return report;
}

template <typename ClockTime>
double elapsedMilliseconds(ClockTime start, ClockTime end = std::chrono::steady_clock::now()) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

AssetType assetTypeForSourcePath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".hdr" || ext == ".exr") return AssetType::HDRI;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".dds" || ext == ".ktx" || ext == ".ktx2" || ext == ".basis") return AssetType::Texture;
    if (ext == ".rtlevel") return AssetType::Scene;
    if (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz") return AssetType::Prefab;
    if (ext == ".gltf" || ext == ".glb") return AssetType::Prefab;
#if RTV_ENABLE_ASSIMP_IMPORTER && RTV_ASSIMP_IMPORTER_AVAILABLE
    if (ext == ".fbx") return AssetType::Prefab;
#endif
    if (ext == ".obj") return AssetType::Mesh;
    if (ext == ".mtl") return AssetType::Material;
    return AssetType::Unknown;
}

std::string assetSourceHashForPath(const std::filesystem::path& path) {
    return pathHashString(path);
}

std::string assetImportSettingsHashForRequest(const AssetImportRequest& request) {
    std::ostringstream key;
    key << request.sourcePath.generic_string()
        << ':' << request.mode
        << ':' << (request.settings.copySourceIntoProject ? 1 : 0)
        << ':' << (request.settings.preserveHierarchy ? 1 : 0)
        << ':' << (request.settings.importMaterials ? 1 : 0)
        << ':' << (request.settings.importTextures ? 1 : 0)
        << ':' << (request.settings.importCameras ? 1 : 0)
        << ':' << (request.settings.importLights ? 1 : 0)
        << ':' << (request.settings.generateTangents ? 1 : 0)
        << ':' << (request.settings.buildBlasCache ? 1 : 0)
        << ':' << (request.settings.generatePrefabAsset ? 1 : 0)
        << ':' << (request.settings.buildCookedPayloadsNow ? 1 : 0)
        << ':' << (request.settings.generateThumbnails ? 1 : 0)
        << ':' << request.settings.unitScale
        << ':' << request.settings.coordinateConversion
        << ':' << request.settings.materialImportMode
        << ':' << request.settings.textureImportMode
        << ':' << request.settings.textureCompression;
    return pathHashString(key.str());
}

NativeTextureFormatSupport nativeTextureFormatSupportForAssetImport(
    const NativeTextureFormatSupport& support,
    const AssetImportSettings& settings) {
    NativeTextureFormatSupport out = support;
    const std::string compression = lowerString(settings.textureCompression);
    if (compression == "preservesource" ||
        compression == "preserve_source" ||
        compression == "preserve-source" ||
        compression == "preserve source" ||
        compression == "none" ||
        compression == "off") {
        out.bc1SrgbSampled = false;
        out.bc1UnormSampled = false;
        out.bc3SrgbSampled = false;
        out.bc3UnormSampled = false;
        out.bc7SrgbSampled = false;
        out.bc7UnormSampled = false;
        out.bc5UnormSampled = false;
        out.bc4UnormSampled = false;
        out.bc6hUfloatSampled = false;
        out.bc6hSfloatSampled = false;
        out.rgba8SrgbSampled = true;
        out.rgba8UnormSampled = true;
        out.rgba16fSampled = true;
        out.platformName += "+asset-import-preserve-source";
    }
    return out;
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
    const AssetImportWorkspace& workspace,
    std::function<void(float, std::string)> progress) {
    const auto workerStart = std::chrono::steady_clock::now();
    auto setProgress = [&](float value, std::string stage) {
        if (progress) {
            progress(value, std::move(stage));
        }
    };
    StagedAssetImportResult result;
    const auto validateStart = std::chrono::steady_clock::now();
    setProgress(0.05f, "Validating source");
    if (request.sourcePath.empty()) {
        result.errors.push_back("Import source path is empty");
        result.workerValidateMs = elapsedMilliseconds(validateStart);
        result.workerTotalMs = elapsedMilliseconds(workerStart);
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::exists(request.sourcePath, ec)) {
        result.errors.push_back("Import source path does not exist: " + request.sourcePath.string());
        result.workerValidateMs = elapsedMilliseconds(validateStart);
        result.workerTotalMs = elapsedMilliseconds(workerStart);
        return result;
    }
    if (workspace.contentRoot.empty() || workspace.cacheRoot.empty() || workspace.registryPath.empty()) {
        result.errors.push_back("Import workspace is incomplete");
        result.workerValidateMs = elapsedMilliseconds(validateStart);
        result.workerTotalMs = elapsedMilliseconds(workerStart);
        return result;
    }
    result.workerValidateMs = elapsedMilliseconds(validateStart);

    const AssetType type = assetTypeForSourcePath(request.sourcePath);
    const std::string name = safeStem(request.sourcePath);
    const std::string sourceExtension = lowerString(request.sourcePath.extension().string());
    const std::filesystem::path destinationFolder = request.destinationFolder.empty()
        ? destinationFolderForType(type)
        : request.destinationFolder;
    const std::filesystem::path importedDir = workspace.contentRoot / destinationFolder / name;
    const std::filesystem::path cacheDir = workspace.cacheRoot / destinationFolder / name;
    const auto directoryStart = std::chrono::steady_clock::now();
    setProgress(0.18f, "Preparing import directories");
    std::filesystem::create_directories(importedDir, ec);
    if (ec) {
        result.errors.push_back("Could not create import destination: " + ec.message());
        result.workerDirectoryMs = elapsedMilliseconds(directoryStart);
        result.workerTotalMs = elapsedMilliseconds(workerStart);
        return result;
    }
    std::filesystem::create_directories(cacheDir, ec);
    if (ec) {
        result.errors.push_back("Could not create import cache folder: " + ec.message());
        result.workerDirectoryMs = elapsedMilliseconds(directoryStart);
        result.workerTotalMs = elapsedMilliseconds(workerStart);
        return result;
    }
    result.workerDirectoryMs = elapsedMilliseconds(directoryStart);

    const bool sourceIsGltf = sourceExtension == ".gltf" || sourceExtension == ".glb";
    const bool sourceIsObj = sourceExtension == ".obj";
    const bool sourceIsMtl = sourceExtension == ".mtl";
    const bool sourceIsFbx = sourceExtension == ".fbx";
    const bool sourceIsUsd = sourceExtension == ".usd" || sourceExtension == ".usda" || sourceExtension == ".usdc" || sourceExtension == ".usdz";
    const bool sourceIsStandaloneTexture = type == AssetType::Texture || type == AssetType::HDRI;
    const std::filesystem::path importedPath = importedDir / (name + importedAssetExtensionForType(type));
    const std::filesystem::path cachePath = cacheDir / (name + ".rtimportcache.json");
    const std::filesystem::path reportPath = importedDir / (name + ".import_report.json");
    if (sourceIsUsd) {
        const std::array<const char*, 7> staleRuntimeSubdirs = {
            "Meshes",
            "Materials",
            "Textures",
            "Skeletons",
            "Animations",
            "AnimationControllers",
            "SkeletalMeshes",
        };
        for (const char* subdir : staleRuntimeSubdirs) {
            const std::filesystem::path cacheSubdir = cacheDir / subdir;
            const std::filesystem::path importedSubdir = importedDir / subdir;
            std::error_code removeEc;
            (void)std::filesystem::remove_all(cacheSubdir, removeEc);
            if (removeEc) {
                result.warnings.push_back("Could not clear stale USD runtime cache folder " + cacheSubdir.generic_string() + ": " + removeEc.message());
            }
            removeEc.clear();
            (void)std::filesystem::remove_all(importedSubdir, removeEc);
            if (removeEc) {
                result.warnings.push_back("Could not clear stale USD imported metadata folder " + importedSubdir.generic_string() + ": " + removeEc.message());
            }
        }
    }
    if (workspace.compatibilityMode) {
        result.warnings.push_back("Imported in no-project compatibility mode; create/open a project for normal asset workflows.");
    }

    const std::filesystem::path originalSourcePath = request.sourcePath;
    std::filesystem::path effectiveSourcePath = request.sourcePath;
    std::filesystem::path copiedSourcePath;
    if (request.settings.copySourceIntoProject) {
        if (workspace.sourceAssetsRoot.empty()) {
            result.warnings.push_back("Copy source into project requested, but no SourceAssets root is available; using external source reference.");
        } else {
            setProgress(0.26f, "Copying source into project");
            const std::filesystem::path sourceImportRoot = workspace.sourceAssetsRoot / destinationFolder / name;
            copiedSourcePath = (sourceImportRoot / request.sourcePath.filename()).lexically_normal();
            std::unordered_set<std::string> copiedKeys;
            if (!copyImportSourceFile(request.sourcePath, copiedSourcePath, result.generatedFiles, result.errors, copiedKeys)) {
                result.workerTotalMs = elapsedMilliseconds(workerStart);
                return result;
            }
            if (sourceExtension == ".gltf") {
                copyGltfExternalReferences(
                    request.sourcePath,
                    copiedSourcePath,
                    sourceImportRoot,
                    result.generatedFiles,
                    result.warnings,
                    result.errors,
                    copiedKeys);
                if (!result.errors.empty()) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
            } else if (sourceIsObj) {
                nlohmann::json objMetadataForCopy = inspectObjSource(request.sourcePath, result.warnings);
                copyObjMaterialLibraries(
                    request.sourcePath,
                    copiedSourcePath,
                    objMetadataForCopy,
                    sourceImportRoot,
                    result.generatedFiles,
                    result.warnings,
                    result.errors,
                    copiedKeys);
                if (!result.errors.empty()) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
            } else if (sourceIsMtl) {
                nlohmann::json mtlMetadataForCopy = inspectMtlSource(request.sourcePath, result.warnings);
                copyMtlTextureReferences(
                    request.sourcePath,
                    copiedSourcePath,
                    mtlMetadataForCopy,
                    sourceImportRoot,
                    result.generatedFiles,
                    result.warnings,
                    result.errors,
                    copiedKeys);
                if (!result.errors.empty()) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
            }
            effectiveSourcePath = copiedSourcePath;
            result.warnings.push_back("Import source copied into project SourceAssets: " + genericRelativeOrValue(copiedSourcePath, workspace.root));
        }
    }

    AssetImportRequest effectiveRequest = request;
    effectiveRequest.sourcePath = effectiveSourcePath;
    const std::string sourceHash = assetSourceHashForPath(effectiveSourcePath);
    const std::string importSettingsHash = assetImportSettingsHashForRequest(effectiveRequest);
    const AssetGuid guid = importedAssetGuidFor(sourceHash, importSettingsHash, assetTypeName(type), 0);
    const std::string effectiveSourceString = effectiveSourcePath.generic_string();
    const std::string originalSourceString = originalSourcePath.generic_string();
    const std::string copiedSourceString = copiedSourcePath.empty() ? std::string{} : copiedSourcePath.generic_string();
    const nlohmann::json sourceControlPolicy = sourceControlPolicyJson(!copiedSourcePath.empty());
    if (sourceIsUsd) {
        traceImport("USD import: begin " + effectiveSourceString);
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

    const NativeTextureFormatSupport importTextureFormatSupport =
        nativeTextureFormatSupportForAssetImport(workspace.nativeTextureFormatSupport, request.settings);
    NativeAssetCooker nativeCooker(importTextureFormatSupport);
    auto nativeCookInput = [&](const AssetGuid& assetGuid, const std::filesystem::path& outputPath, const std::string& displayName) {
        return NativeAssetCookInput{
            .guid = assetGuid,
            .outputPath = outputPath,
            .sourcePath = effectiveSourcePath,
            .displayName = displayName,
            .sourceHash = sourceHash,
            .importSettingsHash = importSettingsHash,
        };
    };
    auto recordNativeCookResult = [&](const NativeAssetCookResult& cook, std::string_view label) -> bool {
        for (const std::string& warning : cook.warnings) {
            result.warnings.push_back(warning);
        }
        if (!cook.success) {
            if (cook.errors.empty()) {
                result.errors.push_back("Native asset cook failed: " + std::string(label));
            } else {
                for (const std::string& error : cook.errors) {
                    result.errors.push_back("Native asset cook failed for " + std::string(label) + ": " + error);
                }
            }
            return false;
        }
        result.generatedFiles.push_back(cook.path);
        return true;
    };

    nlohmann::json placeholder = {
        {"version", 1},
        {"kind", sourceIsGltf ? "ImportedGltfPrefabRoot" : sourceIsFbx ? "ImportedFbxPrefabRoot" : sourceIsUsd ? "ImportedUsdPrefabRoot" : sourceIsObj ? "ImportedObjMesh" : sourceIsMtl ? "ImportedMtlMaterialLibrary" : standaloneAssetKindForType(type)},
        {"guid", guid},
        {"type", assetTypeName(type)},
        {"displayName", name},
        {"sourcePath", effectiveSourceString},
        {"originalSourcePath", originalSourceString},
        {"copiedSourcePath", copiedSourceString},
        {"sourceReferenceMode", copiedSourcePath.empty() ? "ExternalReference" : "CopiedIntoProject"},
        {"sourceControlPolicy", sourceControlPolicy},
        {"mode", request.mode},
        {"nonMutatingSkeleton", true},
    };
    nlohmann::json cache = {
        {"version", 1},
        {"kind", sourceIsFbx ? "FbxImportCacheSummary" : sourceIsUsd ? "UsdImportCacheSummary" : sourceIsObj ? "ObjImportCacheSummary" : sourceIsMtl ? "MtlImportCacheSummary" : importCacheKindForType(type)},
        {"guid", guid},
        {"sourcePath", effectiveSourceString},
        {"originalSourcePath", originalSourceString},
        {"copiedSourcePath", copiedSourceString},
        {"sourceControlPolicy", sourceControlPolicy},
        {"sourceHash", sourceHash},
        {"importSettingsHash", importSettingsHash},
    };
    nlohmann::json runtimePayload;
    nlohmann::json cookedPayloads = nlohmann::json::array();
    nlohmann::json skeletalAnimationMetadata = nlohmann::json::object();
    nlohmann::json collisionLodMetadata = nlohmann::json::object();
    nlohmann::json objMetadata = nlohmann::json::object();
    nlohmann::json mtlMetadata = nlohmann::json::object();
    nlohmann::json fbxMetadata = nlohmann::json::object();
    nlohmann::json usdMetadata = nlohmann::json::object();
    nlohmann::json importerCapabilities = nlohmann::json::object();
    nlohmann::json textureRoleMetadata = nlohmann::json::object();
    nlohmann::json thumbnailMetadata = nlohmann::json::object();
    std::string rootThumbnailPath;
    bool standaloneBasisUnsupported = false;
    bool importFailed = false;

    std::vector<AssetRecord> records;
    std::vector<AssetGuid> rootDependencies;
    if (sourceIsGltf) {
        const auto preflightStart = std::chrono::steady_clock::now();
        setProgress(0.30f, "Checking glTF required extensions");
        const nlohmann::json gltfDoc = loadGltfJsonDocument(effectiveSourcePath, result.warnings);
        const nlohmann::json unsupportedRequiredExtensions = unsupportedRequiredGltfExtensionsForImportReport(gltfDoc);
        if (!unsupportedRequiredExtensions.empty()) {
            skeletalAnimationMetadata = gltfSkeletalAnimationMetadata(effectiveSourcePath, result.warnings);
            collisionLodMetadata = gltfCollisionLodMetadata(effectiveSourcePath, result.warnings);
            importerCapabilities = importerCapabilityReport(
                effectiveSourcePath,
                type,
                request.settings,
                objMetadata,
                mtlMetadata,
                usdMetadata,
                skeletalAnimationMetadata,
                collisionLodMetadata,
                result.warnings);
            const std::string blockMessage = unsupportedRequiredGltfExtensionMessage(unsupportedRequiredExtensions);
            result.errors.push_back(blockMessage + "; runtime import requires a decoder or explicit support before cooked assets can be generated.");
            result.workerInspectMs = elapsedMilliseconds(preflightStart);

            const auto writeStart = std::chrono::steady_clock::now();
            setProgress(0.92f, "Writing blocked import report");
            result.workerTotalMs = elapsedMilliseconds(workerStart);
            nlohmann::json report = {
                {"version", 1},
                {"kind", "ImportReport"},
                {"guid", guid},
                {"sourcePath", effectiveSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"sourceControlPolicy", sourceControlPolicy},
                {"importProvenance", {
                    {"originalSourcePath", originalSourceString},
                    {"effectiveSourcePath", effectiveSourceString},
                    {"copiedSourcePath", copiedSourceString},
                    {"sourceReferenceMode", copiedSourcePath.empty() ? "ExternalReference" : "CopiedIntoProject"},
                    {"importer", importerLabelForType(type)},
                    {"importerVersion", 1},
                    {"mode", request.mode},
                    {"settings", {
                        {"copySourceIntoProject", request.settings.copySourceIntoProject},
                        {"preserveHierarchy", request.settings.preserveHierarchy},
                        {"importMaterials", request.settings.importMaterials},
                        {"importTextures", request.settings.importTextures},
                        {"importCameras", request.settings.importCameras},
                        {"importLights", request.settings.importLights},
                        {"generateTangents", request.settings.generateTangents},
                        {"buildBlasCache", request.settings.buildBlasCache},
                        {"generatePrefabAsset", request.settings.generatePrefabAsset},
                        {"buildCookedPayloadsNow", request.settings.buildCookedPayloadsNow},
                        {"generateThumbnails", request.settings.generateThumbnails},
                        {"unitScale", request.settings.unitScale},
                        {"coordinateConversion", request.settings.coordinateConversion},
                        {"materialImportMode", request.settings.materialImportMode},
                        {"textureImportMode", request.settings.textureImportMode},
                        {"textureCompression", request.settings.textureCompression},
                        {"emissiveScale", request.settings.emissiveScale},
                    }},
                }},
                {"unsupportedRequiredExtensions", unsupportedRequiredExtensions},
                {"runtimePayload", nlohmann::json::object()},
                {"cookedPayloads", nlohmann::json::array()},
                {"importerCapabilities", importerCapabilities.is_null() ? nlohmann::json::object() : importerCapabilities},
                {"skeletalAnimationMetadata", skeletalAnimationMetadata.is_null() ? nlohmann::json::object() : skeletalAnimationMetadata},
                {"collisionLodMetadata", collisionLodMetadata.is_null() ? nlohmann::json::object() : collisionLodMetadata},
                {"dependencyRecords", nlohmann::json::array()},
                {"generatedFiles", nlohmann::json::array()},
                {"warnings", result.warnings},
                {"errors", result.errors},
                {"timings_ms", {
                    {"total", result.workerTotalMs},
                    {"validate", result.workerValidateMs},
                    {"directories", result.workerDirectoryMs},
                    {"inspect", result.workerInspectMs},
                    {"write", 0.0},
                }},
                {"sceneMutation", false},
                {"rendererResourcesCreated", false},
            };
            if (!writeJson(reportPath, report)) {
                result.workerWriteMs = elapsedMilliseconds(writeStart);
                result.workerTotalMs = elapsedMilliseconds(workerStart);
                return result;
            }
            result.workerWriteMs = elapsedMilliseconds(writeStart);
            result.workerTotalMs = elapsedMilliseconds(workerStart);
            report["timings_ms"] = {
                {"total", result.workerTotalMs},
                {"validate", result.workerValidateMs},
                {"directories", result.workerDirectoryMs},
                {"inspect", result.workerInspectMs},
                {"write", result.workerWriteMs},
            };
            if (std::ofstream reportFile(reportPath); reportFile.is_open()) {
                reportFile << report.dump(2);
            }
            result.importReportPath = reportPath;
            return result;
        }
        result.workerInspectMs = elapsedMilliseconds(preflightStart);
    }
    if (sourceIsGltf) {
        try {
            const auto inspectStart = std::chrono::steady_clock::now();
            setProgress(0.35f, "Inspecting and cooking glTF source");
            const std::filesystem::path sceneCachePath = SceneCache::cachePathFor(effectiveSourcePath);
            const bool sceneCacheWasValidBefore = SceneCache::isCacheValid(effectiveSourcePath, sceneCachePath);
            AssetManager importedAssets;
            GltfLoader loader(importedAssets);
            loader.setCacheWritesEnabled(request.settings.buildCookedPayloadsNow);
            loader.setNativeTextureFormatSupport(importTextureFormatSupport);
            SceneAsset scene = loader.loadWithCache(effectiveSourcePath);
            result.workerInspectMs = elapsedMilliseconds(inspectStart);
            const bool sceneCacheExists = std::filesystem::exists(sceneCachePath);
            const std::string sceneCacheHash = fileFingerprintString(sceneCachePath);
            const std::filesystem::path gltfNativeCookDependency = sceneCacheExists ? sceneCachePath : effectiveSourcePath;
            rootThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(effectiveSourcePath, workspace.root) : std::string{};
            thumbnailMetadata = thumbnailMetadataJson("GeneratedSourcePreview", rootThumbnailPath, sourceHash, importSettingsHash, sceneCacheHash);
            if (sceneCacheExists && !sceneCacheWasValidBefore) {
                result.generatedFiles.push_back(sceneCachePath);
            }
            setProgress(0.55f, "Writing texture metadata");
            placeholder["nodeCount"] = scene.nodes.size();
            placeholder["rootNodes"] = scene.rootNodes;
            placeholder["lightCount"] = scene.lights.size();
            placeholder["textureCount"] = importedAssets.textures().size();
            placeholder["materialCount"] = importedAssets.materials().size();
            placeholder["meshCount"] = importedAssets.meshes().size();
            skeletalAnimationMetadata = gltfSkeletalAnimationMetadata(effectiveSourcePath, result.warnings);
            collisionLodMetadata = gltfCollisionLodMetadata(effectiveSourcePath, result.warnings);
            placeholder["skeletalAnimationMetadata"] = skeletalAnimationMetadata;
            placeholder["collisionLodMetadata"] = collisionLodMetadata;
            placeholder["thumbnail"] = thumbnailMetadata;
            runtimePayload = {
                {"kind", "SceneCache"},
                {"cachePath", genericRelativeOrValue(sceneCachePath, workspace.root)},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"payloadHash", sceneCacheHash},
                {"payloadBytes", fileSizeOrZero(sceneCachePath)},
                {"available", sceneCacheExists},
                {"validForSource", sceneCacheExists && SceneCache::isCacheValid(effectiveSourcePath, sceneCachePath)},
                {"counts", {
                    {"textures", importedAssets.textures().size()},
                    {"materials", importedAssets.materials().size()},
                    {"meshes", importedAssets.meshes().size()},
                    {"nodes", scene.nodes.size()},
                    {"lights", scene.lights.size()},
                }},
            };
            placeholder["runtimePayload"] = runtimePayload;
            cookedPayloads.push_back(runtimePayload);

            auto sceneCacheSlicePayload = [&](std::string assetKind, size_t assetIndex, const AssetGuid& assetGuid) {
                nlohmann::json payload = runtimePayload;
                payload["kind"] = "SceneCacheSlice";
                payload["assetKind"] = std::move(assetKind);
                payload["assetIndex"] = assetIndex;
                payload["assetGuid"] = assetGuid;
                payload["parentPayloadKind"] = "SceneCache";
                return payload;
            };

            nlohmann::json nodes = nlohmann::json::array();
            for (size_t i = 0; i < scene.nodes.size(); ++i) {
                const SceneNodeAsset& node = scene.nodes[i];
                nodes.push_back({
                    {"index", i},
                    {"name", node.name.empty() ? ("Node_" + std::to_string(i)) : node.name},
                    {"parent", node.parent},
                    {"transform", transformJsonFromMatrix(node.transform)},
                    {"matrix", matrixJson(node.transform)},
                    {"mesh", node.mesh.valid() ? static_cast<int>(node.mesh.index) : -1},
                    {"morphWeights", node.morphWeights},
                    {"hasCamera", node.hasCamera},
                    {"cameraProjection", node.cameraProjection},
                    {"cameraYfov", node.cameraYfov},
                    {"cameraAspectRatio", node.cameraAspectRatio},
                    {"cameraOrthoXmag", node.cameraOrthoXmag},
                    {"cameraOrthoYmag", node.cameraOrthoYmag},
                    {"cameraNear", node.cameraNear},
                    {"cameraFar", node.cameraFar},
                    {"children", node.children},
                });
            }
            placeholder["sourceHierarchy"] = nodes;

            std::vector<AssetGuid> textureGuids;
            std::vector<std::string> textureThumbnailPaths;
            const auto& textures = importedAssets.textures();
            textureGuids.reserve(textures.size());
            textureThumbnailPaths.reserve(textures.size());
            size_t gltfTexturePersistentCacheHitCount = 0;
            // Authoritative texture roles derived from material slot bindings. This survives the
            // scene cache round-trip (loadWithCache rebuilds textures without authoredRole) so the
            // staged/cached import path still emits the dedicated material role rather than only a
            // filename guess.
            const std::unordered_map<uint32_t, NativeTextureRole> materialDerivedTextureRoles =
                buildTextureRolesFromMaterials(importedAssets.materials());
            for (size_t i = 0; i < textures.size(); ++i) {
                const TextureAsset& texture = textures[i];
                const AssetGuid textureGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Texture", i);
                textureGuids.push_back(textureGuid);
                const std::string textureName = safeStem(texture.name.empty() ? ("Texture_" + std::to_string(i)) : texture.name);
                const std::filesystem::path texturePath = importedDir / "Textures" / (textureName + ".rttexture.json");
                const std::filesystem::path textureCache = cacheDir / "Textures" / (textureName + ".rttexturecache.json");
                const std::filesystem::path nativeTexturePath = cacheDir / "Textures" / (textureName + ".rttexture");
                std::filesystem::create_directories(texturePath.parent_path(), ec);
                std::filesystem::create_directories(textureCache.parent_path(), ec);
                const std::string textureSourcePath = texture.sourcePath.empty() ? effectiveSourceString : texture.sourcePath.generic_string();
                const std::filesystem::path textureRoleSource =
                    texture.sourcePath.empty() || texture.sourcePath == effectiveSourcePath
                        ? std::filesystem::path(texture.name)
                        : texture.sourcePath;
                nlohmann::json textureRole = inferTextureRole(textureRoleSource, AssetType::Texture);
                // Prefer the role authored by the glTF material binding (slot/extension) over the
                // filename heuristic, since the binding is authoritative for what the texture is.
                // The material-derived map survives the scene cache round-trip; texture.authoredRole
                // is the fresh-load fallback when no material binds this texture.
                NativeTextureRole authoredNativeRole = NativeTextureRole::Unknown;
                if (auto roleIt = materialDerivedTextureRoles.find(static_cast<uint32_t>(i));
                    roleIt != materialDerivedTextureRoles.end()) {
                    authoredNativeRole = roleIt->second;
                } else if (!texture.authoredRole.empty() && texture.authoredRole != "unknown") {
                    authoredNativeRole = nativeTextureRoleFromString(texture.authoredRole);
                }
                if (authoredNativeRole != NativeTextureRole::Unknown) {
                    const std::string authoredRoleName = nativeTextureRoleName(authoredNativeRole);
                    textureRole = {
                        {"role", authoredRoleName},
                        {"colorSpace", nativeTextureColorSpaceName(colorSpaceForTextureRole(authoredNativeRole))},
                        {"confidence", "authored_material_binding"},
                        {"matchedToken", authoredRoleName},
                        {"filenameInferredRole", inferTextureRole(textureRoleSource, AssetType::Texture).value("role", std::string("unknown"))},
                    };
                }
                const std::string textureThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(texture.sourcePath, workspace.root) : std::string{};
                textureThumbnailPaths.push_back(textureThumbnailPath);
                const std::filesystem::path textureCacheDependency = texture.sourcePath.empty() ? gltfNativeCookDependency : texture.sourcePath;
                const std::optional<NativeAssetCookResult> reusableTexture =
                    reusableNativeCookResult(nativeTexturePath, NativeAssetKind::Texture, textureGuid, textureCacheDependency);
                const bool texturePersistentCacheReused = reusableTexture.has_value();
                if (texturePersistentCacheReused) {
                    ++gltfTexturePersistentCacheHitCount;
                }
                const NativeAssetCookResult textureCook = reusableTexture.has_value()
                    ? *reusableTexture
                    : nativeCooker.cookTexture(
                        nativeCookInput(textureGuid, nativeTexturePath, textureName),
                        texture,
                        textureRole.value("role", std::string("unknown")));
                if (!recordNativeCookResult(textureCook, textureName)) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
                nlohmann::json texturePayload = nativeCookRuntimePayloadJson(
                    textureCook,
                    NativeAssetKind::Texture,
                    textureGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                texturePayload["assetIndex"] = i;
                texturePayload["sourceTexturePath"] = textureSourcePath;
                texturePayload["textureRole"] = textureRole;
                texturePayload["persistentCacheReused"] = texturePersistentCacheReused;
                const nlohmann::json textureThumbnail = thumbnailMetadataJson("SourceTexturePreview", textureThumbnailPath, sourceHash, importSettingsHash, sceneCacheHash);
                if (request.settings.importTextures) {
                    rootDependencies.push_back(textureGuid);
                    cookedPayloads.push_back(texturePayload);
                    (void)writeJson(texturePath, {
                        {"version", 1},
                        {"kind", "ImportedGltfTexture"},
                        {"guid", textureGuid},
                        {"sourcePath", textureSourcePath},
                        {"rootSourcePath", effectiveSourceString},
                        {"originalRootSourcePath", originalSourceString},
                        {"copiedRootSourcePath", copiedSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", texturePayload},
                        {"thumbnail", textureThumbnail},
                        {"width", texture.width},
                        {"height", texture.height},
                        {"channels", texture.channels},
                        {"colorSpace", textureColorSpaceLabel(texture)},
                        {"textureRole", textureRole},
                        {"textureImportMode", request.settings.textureImportMode},
                        {"textureCompression", request.settings.textureCompression},
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
                    record.sourcePath = textureSourcePath;
                    record.importedPath = genericRelativeOrValue(texturePath, workspace.root);
                    record.cachePath = texturePayload.value("cachePath", genericRelativeOrValue(textureCache, workspace.root));
                    record.thumbnailPath = textureThumbnailPath;
                    record.sourceHash = sourceHash;
                    record.importSettingsHash = importSettingsHash;
                    record.lastModifiedTimestamp = timestampString();
                    record.importSettings = request.settings;
                    record.status = AssetImportStatus::Imported;
                    records.push_back(std::move(record));
                }
            }

            setProgress(0.68f, "Writing material metadata");
            std::vector<AssetGuid> materialGuids;
            const auto& materials = importedAssets.materials();
            materialGuids.reserve(materials.size());
            std::unordered_map<std::string, CookedAssetReuseEntry> gltfCookedMaterialsByContent;
            nlohmann::json gltfMaterialDedupAliases = nlohmann::json::array();
            size_t gltfMaterialContentReuseCount = 0;
            size_t gltfMaterialPersistentCacheHitCount = 0;
            for (size_t i = 0; i < materials.size(); ++i) {
                const MaterialAsset& material = materials[i];
                const AssetGuid candidateMaterialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Material", i);
                const std::string materialName = safeStem(material.name.empty() ? ("Material_" + std::to_string(i)) : material.name);
                const std::string materialContentKey = materialContentDedupKey(material, textureGuids);
                const auto existingMaterial = gltfCookedMaterialsByContent.find(materialContentKey);
                if (existingMaterial != gltfCookedMaterialsByContent.end()) {
                    materialGuids.push_back(existingMaterial->second.guid);
                    ++gltfMaterialContentReuseCount;
                    gltfMaterialDedupAliases.push_back({
                        {"sourceIndex", i},
                        {"sourceName", materialName},
                        {"reusedSourceIndex", existingMaterial->second.sourceIndex},
                        {"reusedName", existingMaterial->second.name},
                        {"guid", existingMaterial->second.guid},
                        {"nativePath", genericRelativeOrValue(existingMaterial->second.nativePath, workspace.root)},
                    });
                    continue;
                }
                const AssetGuid materialGuid = candidateMaterialGuid;
                materialGuids.push_back(materialGuid);
                const std::filesystem::path materialPath = importedDir / "Materials" / (materialName + ".rtmaterial.json");
                const std::filesystem::path nativeMaterialPath = cacheDir / "Materials" / (materialName + ".rtmaterial");
                std::filesystem::create_directories(materialPath.parent_path(), ec);
                nlohmann::json textureDependencies = nlohmann::json::array();
                auto addTextureDependency = [&](TextureAssetHandle handle, const char* role, const char* colorSpace) {
                    if (request.settings.importTextures && handle.valid() && handle.index < textureGuids.size()) {
                        textureDependencies.push_back({{"guid", textureGuids[handle.index]}, {"role", role}, {"colorSpace", colorSpace}});
                    }
                };
                auto thumbnailForTexture = [&](TextureAssetHandle handle) -> std::string {
                    if (handle.valid() && handle.index < textureThumbnailPaths.size()) {
                        return textureThumbnailPaths[handle.index];
                    }
                    return {};
                };
                addTextureDependency(material.baseColorTexture, "baseColor", "sRGB");
                addTextureDependency(material.emissiveTexture, "emissive", "sRGB");
                addTextureDependency(material.normalTexture, "normal", "Linear");
                addTextureDependency(material.metallicRoughnessTexture, "metallicRoughness", "Linear; metallic=B, roughness=G");
                addTextureDependency(material.occlusionTexture, "occlusion", "Linear; occlusion=R");
                addTextureDependency(material.clearcoatTexture, "clearcoat", "Linear");
                addTextureDependency(material.clearcoatRoughnessTexture, "clearcoatRoughness", "Linear");
                addTextureDependency(material.clearcoatNormalTexture, "clearcoatNormal", "Linear");
                addTextureDependency(material.transmissionTexture, "transmission", "Linear");
                addTextureDependency(material.volumeThicknessTexture, "volumeThickness", "Linear; thickness=G");
                addTextureDependency(material.specularTexture, "specular", "Linear");
                addTextureDependency(material.specularColorTexture, "specularColor", "sRGB");
                addTextureDependency(material.sheenColorTexture, "sheenColor", "sRGB");
                addTextureDependency(material.sheenRoughnessTexture, "sheenRoughness", "Linear");
                addTextureDependency(material.iridescenceTexture, "iridescence", "Linear");
                addTextureDependency(material.iridescenceThicknessTexture, "iridescenceThickness", "Linear");
                addTextureDependency(material.anisotropyTexture, "anisotropy", "Linear");
                addTextureDependency(material.opacityTexture, nativeTextureRoleName(NativeTextureRole::Opacity).c_str(), nativeTextureColorSpaceName(NativeTextureColorSpace::Linear).c_str());
                addTextureDependency(material.heightTexture, nativeTextureRoleName(NativeTextureRole::Height).c_str(), nativeTextureColorSpaceName(NativeTextureColorSpace::Linear).c_str());
                std::string materialThumbnailPath;
                if (request.settings.generateThumbnails) {
                    materialThumbnailPath = thumbnailForTexture(material.baseColorTexture);
                    if (materialThumbnailPath.empty()) materialThumbnailPath = thumbnailForTexture(material.emissiveTexture);
                    if (materialThumbnailPath.empty()) materialThumbnailPath = thumbnailForTexture(material.normalTexture);
                    if (materialThumbnailPath.empty()) materialThumbnailPath = thumbnailForTexture(material.metallicRoughnessTexture);
                }
                const nlohmann::json pbrMetadata = materialPbrMetadataJson(material);
                const std::optional<NativeAssetCookResult> reusableMaterial =
                    reusableNativeCookResult(nativeMaterialPath, NativeAssetKind::Material, materialGuid, gltfNativeCookDependency);
                const bool materialPersistentCacheReused = reusableMaterial.has_value();
                if (materialPersistentCacheReused) {
                    ++gltfMaterialPersistentCacheHitCount;
                }
                const NativeAssetCookResult materialCook = reusableMaterial.has_value()
                    ? *reusableMaterial
                    : nativeCooker.cookMaterial(
                        nativeCookInput(materialGuid, nativeMaterialPath, materialName),
                        material,
                        textureGuids);
                if (!recordNativeCookResult(materialCook, materialName)) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
                nlohmann::json materialPayload = nativeCookRuntimePayloadJson(
                    materialCook,
                    NativeAssetKind::Material,
                    materialGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                materialPayload["assetIndex"] = i;
                materialPayload["textureDependencyCount"] = textureDependencies.size();
                materialPayload["persistentCacheReused"] = materialPersistentCacheReused;
                materialPayload["contentDedupKey"] = materialContentKey;
                gltfCookedMaterialsByContent.emplace(materialContentKey, CookedAssetReuseEntry{
                    .guid = materialGuid,
                    .name = materialName,
                    .importedPath = materialPath,
                    .nativePath = nativeMaterialPath,
                    .sourceIndex = i,
                });
                const nlohmann::json materialThumbnail = thumbnailMetadataJson("MaterialTexturePreview", materialThumbnailPath, sourceHash, importSettingsHash, sceneCacheHash);
                if (request.settings.importMaterials) {
                    rootDependencies.push_back(materialGuid);
                    cookedPayloads.push_back(materialPayload);
                    (void)writeJson(materialPath, {
                        {"version", 1},
                        {"kind", "ImportedGltfMaterial"},
                        {"guid", materialGuid},
                        {"sourcePath", effectiveSourceString},
                        {"originalSourcePath", originalSourceString},
                        {"copiedSourcePath", copiedSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", materialPayload},
                        {"thumbnail", materialThumbnail},
                        {"alphaMode", materialAlphaModeLabel(material.alphaMode)},
                        {"doubleSided", material.doubleSided != 0},
                        {"pbr", pbrMetadata},
                        {"materialImportMode", request.settings.materialImportMode},
                        {"metallicRoughnessRule", "glTF metallic-roughness texture uses G=roughness and B=metallic"},
                        {"textureDependencies", textureDependencies},
                    });
                    AssetRecord record;
                    record.guid = materialGuid;
                    record.type = AssetType::Material;
                    record.displayName = materialName;
                    record.sourcePath = effectiveSourceString;
                    record.importedPath = genericRelativeOrValue(materialPath, workspace.root);
                    record.cachePath = materialPayload.value("cachePath", std::string{});
                    record.thumbnailPath = materialThumbnailPath;
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
            }

            setProgress(0.78f, "Writing mesh metadata");
            std::vector<AssetGuid> meshGuids;
            const auto& meshes = importedAssets.meshes();
            meshGuids.reserve(meshes.size());
            std::unordered_map<std::string, CookedAssetReuseEntry> gltfCookedMeshesByPayload;
            nlohmann::json gltfMeshDedupAliases = nlohmann::json::array();
            size_t gltfMeshContentReuseCount = 0;
            size_t gltfMeshPersistentCacheHitCount = 0;
            for (size_t i = 0; i < meshes.size(); ++i) {
                const MeshAsset& mesh = meshes[i];
                const AssetGuid candidateMeshGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Mesh", i);
                const std::string meshName = safeStem((mesh.name.empty() ? "Mesh" : mesh.name) + "_" + std::to_string(i));
                const std::string meshPayloadKey = meshNativePayloadDedupKey(mesh, materialGuids, request.settings.buildBlasCache);
                const auto existingMesh = gltfCookedMeshesByPayload.find(meshPayloadKey);
                if (existingMesh != gltfCookedMeshesByPayload.end()) {
                    meshGuids.push_back(existingMesh->second.guid);
                    ++gltfMeshContentReuseCount;
                    gltfMeshDedupAliases.push_back({
                        {"sourceIndex", i},
                        {"sourceName", meshName},
                        {"reusedSourceIndex", existingMesh->second.sourceIndex},
                        {"reusedName", existingMesh->second.name},
                        {"guid", existingMesh->second.guid},
                        {"nativePath", genericRelativeOrValue(existingMesh->second.nativePath, workspace.root)},
                    });
                    continue;
                }
                const AssetGuid meshGuid = candidateMeshGuid;
                meshGuids.push_back(meshGuid);
                rootDependencies.push_back(meshGuid);
                const std::filesystem::path meshPath = importedDir / "Meshes" / (meshName + ".rtmesh.json");
                const std::filesystem::path meshCache = cacheDir / "Meshes" / (meshName + ".rtmeshcache.json");
                const std::filesystem::path nativeMeshPath = cacheDir / "Meshes" / (meshName + ".rtmesh");
                std::filesystem::create_directories(meshPath.parent_path(), ec);
                std::filesystem::create_directories(meshCache.parent_path(), ec);
                nlohmann::json primitiveMaterials = nlohmann::json::array();
                nlohmann::json primitiveMaterialVariants = nlohmann::json::array();
                nlohmann::json morphTargetPrimitives = nlohmann::json::array();
                size_t morphTargetCount = 0;
                for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex) {
                    const MeshPrimitiveAsset& primitive = mesh.primitives[primitiveIndex];
                    if (request.settings.importMaterials && primitive.material.valid() && primitive.material.index < materialGuids.size()) {
                        primitiveMaterials.push_back(materialGuids[primitive.material.index]);
                    }
                    nlohmann::json variants = nlohmann::json::array();
                    if (request.settings.importMaterials) {
                        for (const auto& variant : primitive.materialVariants) {
                            if (variant.material.valid() && variant.material.index < materialGuids.size()) {
                                variants.push_back({
                                    {"variantIndex", variant.variantIndex},
                                    {"variantName", variant.variantName},
                                    {"materialGuid", materialGuids[variant.material.index]},
                                });
                            }
                        }
                    }
                    primitiveMaterialVariants.push_back(std::move(variants));

                    if (!primitive.morphTargets.empty()) {
                        nlohmann::json targets = nlohmann::json::array();
                        for (size_t targetIndex = 0; targetIndex < primitive.morphTargets.size(); ++targetIndex) {
                            const auto& target = primitive.morphTargets[targetIndex];
                            ++morphTargetCount;
                            nlohmann::json samples = nlohmann::json::array();
                            const size_t sampleCount = std::min<size_t>(3, target.positionDeltas.size());
                            for (size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
                                const glm::vec3& delta = target.positionDeltas[sampleIndex];
                                samples.push_back({
                                    {"vertex", sampleIndex},
                                    {"positionDelta", nlohmann::json::array({delta.x, delta.y, delta.z})},
                                });
                            }
                            targets.push_back({
                                {"index", targetIndex},
                                {"name", target.name},
                                {"hasPositionDeltas", !target.positionDeltas.empty()},
                                {"hasNormalDeltas", !target.normalDeltas.empty()},
                                {"hasTangentDeltas", !target.tangentDeltas.empty()},
                                {"positionDeltaCount", target.positionDeltas.size()},
                                {"normalDeltaCount", target.normalDeltas.size()},
                                {"tangentDeltaCount", target.tangentDeltas.size()},
                                {"samples", samples},
                            });
                        }
                        morphTargetPrimitives.push_back({
                            {"primitiveIndex", primitiveIndex},
                            {"targetCount", primitive.morphTargets.size()},
                            {"targets", targets},
                        });
                    }
                }
                const std::optional<NativeAssetCookResult> reusableMesh =
                    reusableNativeCookResult(nativeMeshPath, NativeAssetKind::Mesh, meshGuid, gltfNativeCookDependency);
                const bool meshPersistentCacheReused = reusableMesh.has_value();
                if (meshPersistentCacheReused) {
                    ++gltfMeshPersistentCacheHitCount;
                }
                const NativeAssetCookResult meshCook = reusableMesh.has_value()
                    ? *reusableMesh
                    : nativeCooker.cookMesh(
                        nativeCookInput(meshGuid, nativeMeshPath, meshName),
                        mesh,
                        materialGuids,
                        request.settings.buildBlasCache);
                if (!recordNativeCookResult(meshCook, meshName)) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
                nlohmann::json meshPayload = nativeCookRuntimePayloadJson(
                    meshCook,
                    NativeAssetKind::Mesh,
                    meshGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                meshPayload["assetIndex"] = i;
                meshPayload["vertexCount"] = mesh.vertices.size();
                meshPayload["indexCount"] = mesh.indices.size();
                meshPayload["primitiveCount"] = mesh.primitives.size();
                meshPayload["persistentCacheReused"] = meshPersistentCacheReused;
                meshPayload["contentDedupKey"] = meshPayloadKey;
                meshPayload["blasCacheRequested"] = request.settings.buildBlasCache;
                size_t skinnedVertexCount = 0;
                nlohmann::json skinningSamples = nlohmann::json::array();
                for (size_t vertexIndex = 0; vertexIndex < mesh.vertices.size(); ++vertexIndex) {
                    const MeshVertex& vertex = mesh.vertices[vertexIndex];
                    const bool hasJoints = vertex.joints.x != 0u || vertex.joints.y != 0u || vertex.joints.z != 0u || vertex.joints.w != 0u;
                    const bool hasWeights = vertex.weights.x > 0.0f || vertex.weights.y > 0.0f || vertex.weights.z > 0.0f || vertex.weights.w > 0.0f;
                    if (hasJoints || hasWeights) {
                        ++skinnedVertexCount;
                        if (skinningSamples.size() < 8) {
                            skinningSamples.push_back({
                                {"vertex", vertexIndex},
                                {"joints", nlohmann::json::array({vertex.joints.x, vertex.joints.y, vertex.joints.z, vertex.joints.w})},
                                {"weights", nlohmann::json::array({vertex.weights.x, vertex.weights.y, vertex.weights.z, vertex.weights.w})},
                            });
                        }
                    }
                }
                gltfCookedMeshesByPayload.emplace(meshPayloadKey, CookedAssetReuseEntry{
                    .guid = meshGuid,
                    .name = meshName,
                    .importedPath = meshPath,
                    .nativePath = nativeMeshPath,
                    .sourceIndex = i,
                });
                cookedPayloads.push_back(meshPayload);
                (void)writeJson(meshPath, {
                    {"version", 1},
                    {"kind", "ImportedGltfMesh"},
                    {"guid", meshGuid},
                    {"sourcePath", effectiveSourceString},
                    {"originalSourcePath", originalSourceString},
                    {"copiedSourcePath", copiedSourceString},
                    {"sourceHash", sourceHash},
                    {"importSettingsHash", importSettingsHash},
                    {"runtimePayload", meshPayload},
                    {"vertexCount", mesh.vertices.size()},
                    {"indexCount", mesh.indices.size()},
                    {"primitiveCount", mesh.primitives.size()},
                    {"defaultMorphWeights", mesh.defaultMorphWeights},
                    {"materialDependencies", primitiveMaterials},
                    {"materialVariantDependencies", primitiveMaterialVariants},
                    {"skinning", {
                        {"hasSkinningPayload", skinnedVertexCount > 0},
                        {"skinnedVertexCount", skinnedVertexCount},
                        {"jointAttribute", "JOINTS_0"},
                        {"weightAttribute", "WEIGHTS_0"},
                        {"samples", skinningSamples},
                        {"runtimeSupport", "shared_runtime_gpu_skinning_supported"},
                        {"animationPlayback", "decoded_keyframes_runtime_playback_supported_when_animation_channels_exist"},
                    }},
                    {"morphTargets", {
                        {"hasMorphTargets", morphTargetCount > 0},
                        {"targetCount", morphTargetCount},
                        {"primitiveCount", morphTargetPrimitives.size()},
                        {"preservedAttributes", nlohmann::json::array({"POSITION", "NORMAL", "TANGENT"})},
                        {"defaultWeightCount", mesh.defaultMorphWeights.size()},
                        {"defaultWeights", mesh.defaultMorphWeights},
                        {"primitives", morphTargetPrimitives},
                        {"runtimeSupport", "shared_runtime_morph_weight_playback_supported"},
                        {"animationPlayback", "decoded_weight_tracks_runtime_playback_supported_when_animation_channels_exist"},
                    }},
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
                record.sourcePath = effectiveSourceString;
                record.importedPath = genericRelativeOrValue(meshPath, workspace.root);
                record.cachePath = meshPayload.value("cachePath", genericRelativeOrValue(meshCache, workspace.root));
                record.thumbnailPath = rootThumbnailPath;
                for (const auto& dep : primitiveMaterials) {
                    record.dependencies.push_back(AssetDependency{dep.get<std::string>(), "material"});
                }
                for (const auto& variants : primitiveMaterialVariants) {
                    for (const auto& variant : variants) {
                        record.dependencies.push_back(AssetDependency{variant.value("materialGuid", std::string{}), "material_variant"});
                    }
                }
                record.sourceHash = sourceHash;
                record.importSettingsHash = importSettingsHash;
                record.lastModifiedTimestamp = timestampString();
                record.importSettings = request.settings;
                record.status = AssetImportStatus::Imported;
                records.push_back(std::move(record));
            }

            setProgress(0.82f, "Writing animation and skeleton metadata");
            nlohmann::json generatedSkeletonAssets = nlohmann::json::array();
            nlohmann::json generatedAnimationAssets = nlohmann::json::array();
            nlohmann::json generatedSkeletalMeshAssets = nlohmann::json::array();
            std::vector<AssetGuid> skeletonGuids;
            std::unordered_map<std::string, AssetGuid> skeletalMeshBindingGuidsByMeshSkin;
            if (skeletalAnimationMetadata.contains("skins") && skeletalAnimationMetadata["skins"].is_array()) {
                skeletonGuids.reserve(skeletalAnimationMetadata["skins"].size());
                for (size_t i = 0; i < skeletalAnimationMetadata["skins"].size(); ++i) {
                    const nlohmann::json& skin = skeletalAnimationMetadata["skins"][i];
                    const AssetGuid skeletonGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Skeleton", i);
                    skeletonGuids.push_back(skeletonGuid);
                    rootDependencies.push_back(skeletonGuid);
                    const std::string authoredSkeletonName = skin.value("name", std::string{});
                    const std::string skeletonName = safeStem(authoredSkeletonName.empty() ? ("Skeleton_" + std::to_string(i)) : authoredSkeletonName);
                    const std::filesystem::path skeletonPath = importedDir / "Skeletons" / (skeletonName + ".rtskeleton.json");
                    const std::filesystem::path nativeSkeletonPath = cacheDir / "Skeletons" / (skeletonName + ".rtskeleton");
                    std::filesystem::create_directories(skeletonPath.parent_path(), ec);
                    nlohmann::json nativeSkeletonMetadata = skin;
                    nativeSkeletonMetadata["assetGuid"] = skeletonGuid;
                    nativeSkeletonMetadata["assetIndex"] = i;
                    const NativeAssetCookResult skeletonCook = nativeCooker.cookMetadataPayload(
                        nativeCookInput(skeletonGuid, nativeSkeletonPath, skeletonName),
                        NativeAssetKind::Skeleton,
                        nativeSkeletonMetadata);
                    if (!recordNativeCookResult(skeletonCook, skeletonName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json skeletonPayload = nativeCookRuntimePayloadJson(
                        skeletonCook,
                        NativeAssetKind::Skeleton,
                        skeletonGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    skeletonPayload["assetIndex"] = i;
                    skeletonPayload["runtimeSupport"] = "cpu_current_pose_skinning_supported";
                    cookedPayloads.push_back(skeletonPayload);
                    (void)writeJson(skeletonPath, {
                        {"version", 1},
                        {"kind", "ImportedGltfSkeleton"},
                        {"guid", skeletonGuid},
                        {"sourcePath", effectiveSourceString},
                        {"originalSourcePath", originalSourceString},
                        {"copiedSourcePath", copiedSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", skeletonPayload},
                        {"skin", skin},
                        {"runtimeSupport", "cpu_current_pose_skinning_supported"},
                    });
                    generatedSkeletonAssets.push_back({
                        {"guid", skeletonGuid},
                        {"name", skeletonName},
                        {"path", genericRelativeOrValue(skeletonPath, workspace.root)},
                        {"jointCount", skin.value("jointCount", 0u)},
                    });

                    AssetRecord record;
                    record.guid = skeletonGuid;
                    record.type = AssetType::Skeleton;
                    record.displayName = skeletonName;
                    record.sourcePath = effectiveSourceString;
                    record.importedPath = genericRelativeOrValue(skeletonPath, workspace.root);
                    record.cachePath = skeletonPayload.value("cachePath", std::string{});
                    record.thumbnailPath = rootThumbnailPath;
                    record.sourceHash = sourceHash;
                    record.importSettingsHash = importSettingsHash;
                    record.lastModifiedTimestamp = timestampString();
                    record.importSettings = request.settings;
                    record.status = AssetImportStatus::Imported;
                    records.push_back(std::move(record));
                }
            }
            if (!skeletonGuids.empty()) {
                std::unordered_set<std::string> emittedBindings;
                for (size_t nodeIndex = 0; nodeIndex < scene.nodes.size(); ++nodeIndex) {
                    const SceneNodeAsset& node = scene.nodes[nodeIndex];
                    if (!node.mesh.valid() || node.mesh.index >= meshGuids.size() || node.skinIndex < 0 || static_cast<size_t>(node.skinIndex) >= skeletonGuids.size()) {
                        continue;
                    }
                    const size_t skinIndex = static_cast<size_t>(node.skinIndex);
                    const std::string bindingKey = std::to_string(node.mesh.index) + ":" + std::to_string(skinIndex);
                    if (!emittedBindings.insert(bindingKey).second) {
                        continue;
                    }
                    const nlohmann::json& skin = skeletalAnimationMetadata["skins"][skinIndex];
                    const AssetGuid meshGuid = meshGuids[node.mesh.index];
                    const AssetGuid skeletonGuid = skeletonGuids[skinIndex];
                    const AssetGuid skeletalMeshGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "SkeletalMesh", generatedSkeletalMeshAssets.size());
                    skeletalMeshBindingGuidsByMeshSkin[bindingKey] = skeletalMeshGuid;
                    rootDependencies.push_back(skeletalMeshGuid);

                    const std::string meshName = node.mesh.index < meshes.size() && !meshes[node.mesh.index].name.empty()
                        ? meshes[node.mesh.index].name
                        : (node.name.empty() ? ("Mesh_" + std::to_string(node.mesh.index)) : node.name);
                    const std::string bindingName = safeStem(meshName + "_Skin" + std::to_string(skinIndex));
                    const std::filesystem::path bindingPath = importedDir / "Skeletons" / (bindingName + ".rtskeletalmesh.json");
                    const std::filesystem::path nativeBindingPath = cacheDir / "Skeletons" / (bindingName + ".rtskeletalmesh");
                    std::filesystem::create_directories(bindingPath.parent_path(), ec);

                    std::vector<uint32_t> jointRemap;
                    const uint32_t jointCount = static_cast<uint32_t>(skin.value("jointCount", 0u));
                    jointRemap.reserve(jointCount);
                    for (uint32_t joint = 0; joint < jointCount; ++joint) {
                        jointRemap.push_back(joint);
                    }
                    nlohmann::json bindMetadata = {
                        {"sourceFormat", "glTF"},
                        {"sourceNodeIndex", nodeIndex},
                        {"meshIndex", node.mesh.index},
                        {"skinIndex", skinIndex},
                        {"meshGuid", meshGuid},
                        {"skeletonGuid", skeletonGuid},
                        {"skinName", skin.value("name", std::string{})},
                        {"jointCount", jointCount},
                        {"runtimeSupport", "binding_decode_gpu_skinning_runtime_supported"},
                    };
                    const NativeAssetCookResult bindingCook = nativeCooker.cookSkeletalMeshBinding(
                        nativeCookInput(skeletalMeshGuid, nativeBindingPath, bindingName),
                        meshGuid,
                        skeletonGuid,
                        jointRemap,
                        bindMetadata);
                    if (!recordNativeCookResult(bindingCook, bindingName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json bindingPayload = nativeCookRuntimePayloadJson(
                        bindingCook,
                        NativeAssetKind::SkeletalMesh,
                        skeletalMeshGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    bindingPayload["meshGuid"] = meshGuid;
                    bindingPayload["skeletonGuid"] = skeletonGuid;
                    bindingPayload["sourceNodeIndex"] = nodeIndex;
                    bindingPayload["meshIndex"] = node.mesh.index;
                    bindingPayload["skinIndex"] = skinIndex;
                    bindingPayload["jointRemapCount"] = jointRemap.size();
                    bindingPayload["runtimeSupport"] = "binding_decode_gpu_skinning_runtime_supported";
                    cookedPayloads.push_back(bindingPayload);

                    (void)writeJson(bindingPath, {
                        {"version", 1},
                        {"kind", "ImportedGltfSkeletalMeshBinding"},
                        {"guid", skeletalMeshGuid},
                        {"sourcePath", effectiveSourceString},
                        {"originalSourcePath", originalSourceString},
                        {"copiedSourcePath", copiedSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", bindingPayload},
                        {"meshGuid", meshGuid},
                        {"skeletonGuid", skeletonGuid},
                        {"sourceNodeIndex", nodeIndex},
                        {"meshIndex", node.mesh.index},
                        {"skinIndex", skinIndex},
                        {"jointRemap", jointRemap},
                        {"runtimeSupport", "binding_decode_gpu_skinning_runtime_supported"},
                    });
                    generatedSkeletalMeshAssets.push_back({
                        {"guid", skeletalMeshGuid},
                        {"name", bindingName},
                        {"path", genericRelativeOrValue(bindingPath, workspace.root)},
                        {"meshGuid", meshGuid},
                        {"skeletonGuid", skeletonGuid},
                        {"jointRemapCount", jointRemap.size()},
                    });

                    AssetRecord record;
                    record.guid = skeletalMeshGuid;
                    record.type = AssetType::SkeletalMesh;
                    record.displayName = bindingName;
                    record.sourcePath = effectiveSourceString;
                    record.importedPath = genericRelativeOrValue(bindingPath, workspace.root);
                    record.cachePath = bindingPayload.value("cachePath", std::string{});
                    record.thumbnailPath = rootThumbnailPath;
                    record.dependencies.push_back(AssetDependency{meshGuid, "mesh"});
                    record.dependencies.push_back(AssetDependency{skeletonGuid, "skeleton"});
                    record.sourceHash = sourceHash;
                    record.importSettingsHash = importSettingsHash;
                    record.lastModifiedTimestamp = timestampString();
                    record.importSettings = request.settings;
                    record.status = AssetImportStatus::Imported;
                    records.push_back(std::move(record));
                }
            }
            if (skeletalAnimationMetadata.contains("animations") && skeletalAnimationMetadata["animations"].is_array()) {
                for (size_t i = 0; i < skeletalAnimationMetadata["animations"].size(); ++i) {
                    const nlohmann::json& animation = skeletalAnimationMetadata["animations"][i];
                    const AssetGuid animationGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Animation", i);
                    rootDependencies.push_back(animationGuid);
                    const std::string authoredAnimationName = animation.value("name", std::string{});
                    const std::string animationName = safeStem(authoredAnimationName.empty() ? ("Animation_" + std::to_string(i)) : authoredAnimationName);
                    const std::filesystem::path animationPath = importedDir / "Animations" / (animationName + ".rtanim.json");
                    const std::filesystem::path nativeAnimationPath = cacheDir / "Animations" / (animationName + ".rtanim");
                    std::filesystem::create_directories(animationPath.parent_path(), ec);
                    nlohmann::json nativeAnimationMetadata = animation;
                    nativeAnimationMetadata["assetGuid"] = animationGuid;
                    nativeAnimationMetadata["assetIndex"] = i;
                    const NativeAssetCookResult animationCook = nativeCooker.cookMetadataPayload(
                        nativeCookInput(animationGuid, nativeAnimationPath, animationName),
                        NativeAssetKind::Animation,
                        nativeAnimationMetadata);
                    if (!recordNativeCookResult(animationCook, animationName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json animationPayload = nativeCookRuntimePayloadJson(
                        animationCook,
                        NativeAssetKind::Animation,
                        animationGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    animationPayload["assetIndex"] = i;
                    animationPayload["runtimeSupport"] = "decoded_keyframes_runtime_playback_supported";
                    cookedPayloads.push_back(animationPayload);
                    (void)writeJson(animationPath, {
                        {"version", 1},
                        {"kind", "ImportedGltfAnimation"},
                        {"guid", animationGuid},
                        {"sourcePath", effectiveSourceString},
                        {"originalSourcePath", originalSourceString},
                        {"copiedSourcePath", copiedSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", animationPayload},
                        {"animation", animation},
                        {"runtimeSupport", "decoded_keyframes_runtime_playback_supported"},
                    });
                    generatedAnimationAssets.push_back({
                        {"guid", animationGuid},
                        {"name", animationName},
                        {"path", genericRelativeOrValue(animationPath, workspace.root)},
                        {"channelCount", animation.value("channelCount", 0u)},
                        {"samplerCount", animation.value("samplerCount", 0u)},
                        {"trackCount", animation.contains("tracks") && animation["tracks"].is_array() ? animation["tracks"].size() : 0},
                        {"decodedChannelCount", animation.value("decodedChannelCount", 0u)},
                        {"decodedKeyframeCount", animation.value("decodedKeyframeCount", 0u)},
                        {"rootMotionCandidateCount", animation.value("rootMotionCandidateCount", 0u)},
                    });

                    AssetRecord record;
                    record.guid = animationGuid;
                    record.type = AssetType::Animation;
                    record.displayName = animationName;
                    record.sourcePath = effectiveSourceString;
                    record.importedPath = genericRelativeOrValue(animationPath, workspace.root);
                    record.cachePath = animationPayload.value("cachePath", std::string{});
                    record.thumbnailPath = rootThumbnailPath;
                    record.sourceHash = sourceHash;
                    record.importSettingsHash = importSettingsHash;
                    record.lastModifiedTimestamp = timestampString();
                    record.importSettings = request.settings;
                    record.status = AssetImportStatus::Imported;
                    records.push_back(std::move(record));
                }
            }
            placeholder["skeletonAssets"] = generatedSkeletonAssets;
            placeholder["animationAssets"] = generatedAnimationAssets;
            placeholder["skeletalMeshAssets"] = generatedSkeletalMeshAssets;

            setProgress(0.86f, "Writing prefab metadata");
            std::unordered_map<int32_t, const SceneLightAsset*> lightByNode;
            for (const SceneLightAsset& light : scene.lights) {
                if (light.nodeIndex >= 0 && static_cast<size_t>(light.nodeIndex) < scene.nodes.size()) {
                    lightByNode.try_emplace(light.nodeIndex, &light);
                }
            }
            nlohmann::json prefabNodes = nlohmann::json::array();
            for (size_t i = 0; i < scene.nodes.size(); ++i) {
                const SceneNodeAsset& node = scene.nodes[i];
                const auto lightIt = lightByNode.find(static_cast<int32_t>(i));
                const SceneLightAsset* light = lightIt != lightByNode.end() ? lightIt->second : nullptr;
                nlohmann::json materialGuidsForNode = nlohmann::json::array();
                nlohmann::json materialVariantGuidsForNode = nlohmann::json::array();
                if (request.settings.importMaterials && node.mesh.valid() && node.mesh.index < meshes.size()) {
                    for (const MeshPrimitiveAsset& primitive : meshes[node.mesh.index].primitives) {
                        if (primitive.material.valid() && primitive.material.index < materialGuids.size()) {
                            materialGuidsForNode.push_back(materialGuids[primitive.material.index]);
                        }
                        for (const auto& variant : primitive.materialVariants) {
                            if (variant.material.valid() && variant.material.index < materialGuids.size()) {
                                materialVariantGuidsForNode.push_back({
                                    {"variantIndex", variant.variantIndex},
                                    {"variantName", variant.variantName},
                                    {"materialGuid", materialGuids[variant.material.index]},
                                });
                            }
                        }
                    }
                }
                std::string skeletalMeshGuidForNode;
                if (node.mesh.valid() && node.skinIndex >= 0) {
                    const std::string bindingKey = std::to_string(node.mesh.index) + ":" + std::to_string(node.skinIndex);
                    const auto bindingIt = skeletalMeshBindingGuidsByMeshSkin.find(bindingKey);
                    if (bindingIt != skeletalMeshBindingGuidsByMeshSkin.end()) {
                        skeletalMeshGuidForNode = bindingIt->second;
                    }
                }
                prefabNodes.push_back({
                    {"index", i},
                    {"sourceNodeIndex", i},
                    {"name", node.name.empty() ? ("Node_" + std::to_string(i)) : node.name},
                    {"parent", node.parent},
                    {"transform", transformJsonFromMatrix(node.transform)},
                    {"matrix", matrixJson(node.transform)},
                    {"children", node.children},
                    {"mesh", node.mesh.valid() ? static_cast<int>(node.mesh.index) : -1},
                    {"meshGuid", node.mesh.valid() && node.mesh.index < meshGuids.size() ? meshGuids[node.mesh.index] : std::string{}},
                    {"skinIndex", node.skinIndex},
                    {"skeletalMeshGuid", skeletalMeshGuidForNode},
                    {"morphWeights", node.morphWeights},
                    {"materialGuids", materialGuidsForNode},
                    {"materialVariantGuids", materialVariantGuidsForNode},
                    {"hasCamera", node.hasCamera},
                    {"cameraProjection", node.cameraProjection},
                    {"cameraYfov", node.cameraYfov},
                    {"cameraAspectRatio", node.cameraAspectRatio},
                    {"cameraOrthoXmag", node.cameraOrthoXmag},
                    {"cameraOrthoYmag", node.cameraOrthoYmag},
                    {"cameraNear", node.cameraNear},
                    {"cameraFar", node.cameraFar},
                    {"hasLight", light != nullptr},
                    {"lightType", light != nullptr ? light->type : 1u},
                    {"lightColor", light != nullptr ? vec3Json(light->color) : vec3Json(glm::vec3{1.0f})},
                    {"lightIntensity", light != nullptr ? light->intensity : 1.0f},
                    {"lightSizeOrRadius", light != nullptr ? light->sizeOrRadius : 1.0f},
                    {"lightInnerConeRadians", light != nullptr ? light->innerConeRadians : 0.35f},
                    {"lightOuterConeRadians", light != nullptr ? light->outerConeRadians : 0.70f},
                    {"lightEnabled", light != nullptr ? light->enabled : true},
                });
            }
            placeholder["sourceHierarchy"] = prefabNodes;
            placeholder["prefab"] = {
                {"version", 1},
                {"guid", guid},
                {"name", name},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"runtimePayload", runtimePayload},
                {"rootNodes", scene.rootNodes},
                {"nodes", prefabNodes},
            };

            const nlohmann::json gltfMaterialDeduplication = {
                {"sourceMaterialCount", materials.size()},
                {"uniqueMaterialCount", gltfCookedMaterialsByContent.size()},
                {"reusedMaterialCount", gltfMaterialContentReuseCount},
                {"persistentCacheHitCount", gltfMaterialPersistentCacheHitCount},
                {"aliases", gltfMaterialDedupAliases},
            };
            const nlohmann::json gltfMeshDeduplication = {
                {"sourceMeshCount", meshes.size()},
                {"uniqueMeshCount", gltfCookedMeshesByPayload.size()},
                {"reusedMeshCount", gltfMeshContentReuseCount},
                {"persistentCacheHitCount", gltfMeshPersistentCacheHitCount},
                {"dedupKeyIncludesMaterialSlots", true},
                {"localBvhCacheRequested", request.settings.buildBlasCache},
                {"aliases", gltfMeshDedupAliases},
            };
            runtimePayload["texturePersistentCacheHitCount"] = gltfTexturePersistentCacheHitCount;
            runtimePayload["materialDeduplication"] = gltfMaterialDeduplication;
            runtimePayload["meshDeduplication"] = gltfMeshDeduplication;
            if (!cookedPayloads.empty()) {
                cookedPayloads[0] = runtimePayload;
            }
            placeholder["runtimePayload"] = runtimePayload;
            placeholder["texturePersistentCacheHitCount"] = gltfTexturePersistentCacheHitCount;
            placeholder["materialDeduplication"] = gltfMaterialDeduplication;
            placeholder["meshDeduplication"] = gltfMeshDeduplication;

            cache["runtimePayload"] = runtimePayload;
            cache["cookedPayloads"] = cookedPayloads;
            cache["thumbnail"] = thumbnailMetadata;
            cache["textureCount"] = textures.size();
            cache["materialCount"] = materials.size();
            cache["meshCount"] = meshes.size();
            cache["texturePersistentCacheHitCount"] = gltfTexturePersistentCacheHitCount;
            cache["materialDeduplication"] = gltfMaterialDeduplication;
            cache["meshDeduplication"] = gltfMeshDeduplication;
            cache["nodeCount"] = scene.nodes.size();
            cache["skeletalAnimationMetadata"] = skeletalAnimationMetadata;
            cache["skeletonAssets"] = generatedSkeletonAssets;
            cache["animationAssets"] = generatedAnimationAssets;
            cache["skeletalMeshAssets"] = generatedSkeletalMeshAssets;
            cache["collisionLodMetadata"] = collisionLodMetadata;
        } catch (const std::exception& ex) {
            if (result.workerInspectMs <= 0.0) {
                result.workerInspectMs = elapsedMilliseconds(workerStart) - result.workerValidateMs - result.workerDirectoryMs;
            }
            result.errors.push_back(std::string("glTF import inspection failed: ") + ex.what());
            result.workerTotalMs = elapsedMilliseconds(workerStart);
            return result;
        }
    } else if (sourceIsFbx) {
        const auto inspectStart = std::chrono::steady_clock::now();
        setProgress(0.42f, "Parsing static FBX source");
        FbxStaticImportData fbxData = loadFbxStaticScene(effectiveSourcePath, name, importTextureFormatSupport, request.settings.emissiveScale);
        for (const std::string& warning : fbxData.warnings) {
            result.warnings.push_back(warning);
        }
        if (!fbxData.supported || !fbxData.errors.empty()) {
            for (const std::string& error : fbxData.errors) {
                result.warnings.push_back(error);
            }
        }
        result.workerInspectMs = elapsedMilliseconds(inspectStart);
        fbxMetadata = fbxData.diagnostics;
        rootThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(effectiveSourcePath, workspace.root) : std::string{};
        thumbnailMetadata = thumbnailMetadataJson("GeneratedSourcePreview", rootThumbnailPath, sourceHash, importSettingsHash);

        if (fbxData.supported && fbxData.errors.empty()) {
            setProgress(0.52f, "Cooking FBX texture assets");
            nlohmann::json generatedSkeletonAssets = nlohmann::json::array();
            nlohmann::json generatedAnimationAssets = nlohmann::json::array();
            nlohmann::json generatedAnimationControllerAssets = nlohmann::json::array();
            std::vector<AssetGuid> textureGuids;
            textureGuids.reserve(fbxData.assets.textures().size());
            size_t fbxTexturePersistentCacheHitCount = 0;
            const auto& textures = fbxData.assets.textures();
            for (size_t i = 0; i < textures.size(); ++i) {
                const TextureAsset& texture = textures[i];
                const AssetGuid textureGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "FbxTexture", i);
                textureGuids.push_back(textureGuid);
                rootDependencies.push_back(textureGuid);
                const std::string textureName = safeStem(texture.name.empty() ? ("FbxTexture_" + std::to_string(i)) : texture.name);
                const std::filesystem::path texturePath = importedDir / "Textures" / (textureName + ".rttexture.json");
                const std::filesystem::path nativeTexturePath = cacheDir / "Textures" / (textureName + ".rttexture");
                std::filesystem::create_directories(texturePath.parent_path(), ec);
                std::filesystem::create_directories(nativeTexturePath.parent_path(), ec);
                const std::string textureRole = i < fbxData.textureRoles.size() && !fbxData.textureRoles[i].empty()
                    ? fbxData.textureRoles[i]
                    : inferTextureRole(texture.sourcePath.empty() ? std::filesystem::path(textureName) : texture.sourcePath, AssetType::Texture).value("role", std::string("unknown"));
                const std::filesystem::path textureCacheDependency = texture.sourcePath.empty() ? effectiveSourcePath : texture.sourcePath;
                const std::optional<NativeAssetCookResult> reusableTexture =
                    reusableNativeCookResult(nativeTexturePath, NativeAssetKind::Texture, textureGuid, textureCacheDependency);
                const bool texturePersistentCacheReused = reusableTexture.has_value();
                if (texturePersistentCacheReused) {
                    ++fbxTexturePersistentCacheHitCount;
                }
                const NativeAssetCookResult textureCook = reusableTexture.has_value()
                    ? *reusableTexture
                    : nativeCooker.cookTexture(
                        nativeCookInput(textureGuid, nativeTexturePath, textureName),
                        texture,
                        textureRole);
                if (!recordNativeCookResult(textureCook, textureName)) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
                nlohmann::json texturePayload = nativeCookRuntimePayloadJson(
                    textureCook,
                    NativeAssetKind::Texture,
                    textureGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                texturePayload["assetIndex"] = i;
                texturePayload["kind"] = "FbxTexturePayload";
                texturePayload["sourceTexturePath"] = texture.sourcePath.generic_string();
                texturePayload["persistentCacheReused"] = texturePersistentCacheReused;
                texturePayload["textureRole"] = {
                    {"role", textureRole},
                    {"source", "fbxAssimpMaterialTexture"},
                    {"colorSpace", textureColorSpaceLabel(texture)},
                };
                cookedPayloads.push_back(texturePayload);
                (void)writeJson(texturePath, {
                    {"version", 1},
                    {"kind", "ImportedFbxTexture"},
                    {"guid", textureGuid},
                    {"sourcePath", texture.sourcePath.generic_string()},
                    {"rootSourcePath", effectiveSourceString},
                    {"originalRootSourcePath", originalSourceString},
                    {"copiedRootSourcePath", copiedSourceString},
                    {"sourceHash", sourceHash},
                    {"importSettingsHash", importSettingsHash},
                    {"runtimePayload", texturePayload},
                    {"width", texture.width},
                    {"height", texture.height},
                    {"channels", texture.channels},
                    {"colorSpace", textureColorSpaceLabel(texture)},
                    {"textureRole", texturePayload["textureRole"]},
                });

                AssetRecord textureRecord;
                textureRecord.guid = textureGuid;
                textureRecord.type = AssetType::Texture;
                textureRecord.displayName = textureName;
                textureRecord.sourcePath = texture.sourcePath.generic_string();
                textureRecord.importedPath = genericRelativeOrValue(texturePath, workspace.root);
                textureRecord.cachePath = texturePayload.value("cachePath", std::string{});
                textureRecord.sourceHash = sourceHash;
                textureRecord.importSettingsHash = importSettingsHash;
                textureRecord.lastModifiedTimestamp = timestampString();
                textureRecord.importSettings = request.settings;
                textureRecord.status = AssetImportStatus::Imported;
                records.push_back(std::move(textureRecord));
            }

            setProgress(0.58f, "Cooking FBX material assets");
            std::vector<AssetGuid> materialGuids;
            materialGuids.reserve(fbxData.assets.materials().size());
            const auto& materials = fbxData.assets.materials();
            std::unordered_map<std::string, CookedAssetReuseEntry> fbxCookedMaterialsByContent;
            nlohmann::json fbxMaterialDedupAliases = nlohmann::json::array();
            size_t fbxMaterialContentReuseCount = 0;
            size_t fbxMaterialPersistentCacheHitCount = 0;
            for (size_t i = 0; i < materials.size(); ++i) {
                const MaterialAsset& material = materials[i];
                const AssetGuid candidateMaterialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Material", i);
                const std::string materialName = safeStem(material.name.empty() ? ("FbxMaterial_" + std::to_string(i)) : material.name);
                const std::string materialContentKey = materialContentDedupKey(material, textureGuids);
                const auto existingMaterial = fbxCookedMaterialsByContent.find(materialContentKey);
                if (existingMaterial != fbxCookedMaterialsByContent.end()) {
                    materialGuids.push_back(existingMaterial->second.guid);
                    ++fbxMaterialContentReuseCount;
                    fbxMaterialDedupAliases.push_back({
                        {"sourceIndex", i},
                        {"sourceName", materialName},
                        {"reusedSourceIndex", existingMaterial->second.sourceIndex},
                        {"reusedName", existingMaterial->second.name},
                        {"guid", existingMaterial->second.guid},
                        {"nativePath", genericRelativeOrValue(existingMaterial->second.nativePath, workspace.root)},
                    });
                    continue;
                }
                const AssetGuid materialGuid = candidateMaterialGuid;
                materialGuids.push_back(materialGuid);
                const std::filesystem::path materialPath = importedDir / "Materials" / (materialName + ".rtmaterial.json");
                const std::filesystem::path nativeMaterialPath = cacheDir / "Materials" / (materialName + ".rtmaterial");
                std::filesystem::create_directories(materialPath.parent_path(), ec);
                std::filesystem::create_directories(nativeMaterialPath.parent_path(), ec);
                nlohmann::json textureDependencies = nlohmann::json::array();
                auto addTextureDependency = [&](TextureAssetHandle handle, const char* role, const char* colorSpace) {
                    if (handle.valid() && handle.index < textureGuids.size()) {
                        textureDependencies.push_back({{"guid", textureGuids[handle.index]}, {"role", role}, {"colorSpace", colorSpace}});
                    }
                };
                addTextureDependency(material.baseColorTexture, "baseColor", "sRGB");
                addTextureDependency(material.emissiveTexture, "emissive", "sRGB");
                addTextureDependency(material.normalTexture, "normal", "Linear");
                addTextureDependency(material.metallicRoughnessTexture, "metallicRoughness", "Linear");
                addTextureDependency(material.occlusionTexture, "occlusion", "Linear");
                addTextureDependency(material.specularTexture, "specular", "Linear");
                addTextureDependency(material.specularColorTexture, "specularColor", "sRGB");
                addTextureDependency(material.opacityTexture, "opacity", "Linear");
                addTextureDependency(material.heightTexture, "height", "Linear");
                const std::optional<NativeAssetCookResult> reusableMaterial =
                    reusableNativeCookResult(nativeMaterialPath, NativeAssetKind::Material, materialGuid, effectiveSourcePath);
                const bool materialPersistentCacheReused = reusableMaterial.has_value();
                if (materialPersistentCacheReused) {
                    ++fbxMaterialPersistentCacheHitCount;
                }
                const NativeAssetCookResult materialCook = reusableMaterial.has_value()
                    ? *reusableMaterial
                    : nativeCooker.cookMaterial(
                        nativeCookInput(materialGuid, nativeMaterialPath, materialName),
                        material,
                        textureGuids);
                if (!recordNativeCookResult(materialCook, materialName)) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
                nlohmann::json materialPayload = nativeCookRuntimePayloadJson(
                    materialCook,
                    NativeAssetKind::Material,
                    materialGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                materialPayload["assetIndex"] = i;
                materialPayload["textureDependencyCount"] = textureDependencies.size();
                materialPayload["persistentCacheReused"] = materialPersistentCacheReused;
                materialPayload["contentDedupKey"] = materialContentKey;
                fbxCookedMaterialsByContent.emplace(materialContentKey, CookedAssetReuseEntry{
                    .guid = materialGuid,
                    .name = materialName,
                    .importedPath = materialPath,
                    .nativePath = nativeMaterialPath,
                    .sourceIndex = i,
                });
                cookedPayloads.push_back(materialPayload);
                rootDependencies.push_back(materialGuid);
                (void)writeJson(materialPath, {
                    {"version", 1},
                    {"kind", "ImportedFbxMaterial"},
                    {"guid", materialGuid},
                    {"sourcePath", effectiveSourceString},
                    {"originalSourcePath", originalSourceString},
                    {"copiedSourcePath", copiedSourceString},
                    {"sourceHash", sourceHash},
                    {"importSettingsHash", importSettingsHash},
                    {"runtimePayload", materialPayload},
                    {"alphaMode", materialAlphaModeLabel(material.alphaMode)},
                    {"pbr", materialPbrMetadataJson(material)},
                    {"textureDependencies", textureDependencies},
                    {"conversion", {
                        {"schema", "FbxAssimpMaterialConversionV1"},
                        {"lossy", true},
                        {"rules", nlohmann::json::array({"diffuse/baseColor->baseColor", "baseColor alpha->opacity", "emissive->emissive", "normal DirectX/OpenGL convention preserved", "Bistro/ORCA specular packed R=occlusion G=roughness B=metalness when source README declares it", "specular/shininess fallback->specular/roughness", "opacity texture->opacity", "twoSided->doubleSided", "external texture references -> native texture GUID slots"})},
                    }},
                });

                AssetRecord materialRecord;
                materialRecord.guid = materialGuid;
                materialRecord.type = AssetType::Material;
                materialRecord.displayName = materialName;
                materialRecord.sourcePath = effectiveSourceString;
                materialRecord.importedPath = genericRelativeOrValue(materialPath, workspace.root);
                materialRecord.cachePath = materialPayload.value("cachePath", std::string{});
                materialRecord.sourceHash = sourceHash;
                materialRecord.importSettingsHash = importSettingsHash;
                materialRecord.lastModifiedTimestamp = timestampString();
                materialRecord.importSettings = request.settings;
                materialRecord.status = AssetImportStatus::Imported;
                for (const auto& dep : textureDependencies) {
                    materialRecord.dependencies.push_back(AssetDependency{dep.value("guid", std::string{}), dep.value("role", std::string{})});
                }
                records.push_back(std::move(materialRecord));
            }

            setProgress(0.70f, "Cooking FBX mesh assets");
            std::vector<AssetGuid> meshGuids;
            meshGuids.reserve(fbxData.assets.meshes().size());
            std::string primaryMeshCachePath;
            const auto& meshes = fbxData.assets.meshes();
            std::unordered_map<std::string, CookedAssetReuseEntry> fbxCookedMeshesByPayload;
            nlohmann::json fbxMeshDedupAliases = nlohmann::json::array();
            size_t fbxMeshContentReuseCount = 0;
            size_t fbxMeshPersistentCacheHitCount = 0;
            for (size_t i = 0; i < meshes.size(); ++i) {
                const MeshAsset& mesh = meshes[i];
                const AssetGuid candidateMeshGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Mesh", i);
                const std::string meshName = safeStem((mesh.name.empty() ? "FbxMesh" : mesh.name) + "_" + std::to_string(i));
                const std::string meshPayloadKey = meshNativePayloadDedupKey(mesh, materialGuids, request.settings.buildBlasCache);
                const auto existingMesh = fbxCookedMeshesByPayload.find(meshPayloadKey);
                if (existingMesh != fbxCookedMeshesByPayload.end()) {
                    meshGuids.push_back(existingMesh->second.guid);
                    ++fbxMeshContentReuseCount;
                    fbxMeshDedupAliases.push_back({
                        {"sourceIndex", i},
                        {"sourceName", meshName},
                        {"reusedSourceIndex", existingMesh->second.sourceIndex},
                        {"reusedName", existingMesh->second.name},
                        {"guid", existingMesh->second.guid},
                        {"nativePath", genericRelativeOrValue(existingMesh->second.nativePath, workspace.root)},
                    });
                    continue;
                }
                const AssetGuid meshGuid = candidateMeshGuid;
                meshGuids.push_back(meshGuid);
                const std::filesystem::path meshPath = importedDir / "Meshes" / (meshName + ".rtmesh.json");
                const std::filesystem::path nativeMeshPath = cacheDir / "Meshes" / (meshName + ".rtmesh");
                std::filesystem::create_directories(meshPath.parent_path(), ec);
                std::filesystem::create_directories(nativeMeshPath.parent_path(), ec);
                const std::optional<NativeAssetCookResult> reusableMesh =
                    reusableNativeCookResult(nativeMeshPath, NativeAssetKind::Mesh, meshGuid, effectiveSourcePath);
                const bool meshPersistentCacheReused = reusableMesh.has_value();
                if (meshPersistentCacheReused) {
                    ++fbxMeshPersistentCacheHitCount;
                }
                const NativeAssetCookResult meshCook = reusableMesh.has_value()
                    ? *reusableMesh
                    : nativeCooker.cookMesh(
                        nativeCookInput(meshGuid, nativeMeshPath, meshName),
                        mesh,
                        materialGuids,
                        request.settings.buildBlasCache);
                if (!recordNativeCookResult(meshCook, meshName)) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
                nlohmann::json meshPayload = nativeCookRuntimePayloadJson(
                    meshCook,
                    NativeAssetKind::Mesh,
                    meshGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                meshPayload["assetIndex"] = i;
                meshPayload["kind"] = "FbxStaticMeshPayload";
                meshPayload["persistentCacheReused"] = meshPersistentCacheReused;
                meshPayload["contentDedupKey"] = meshPayloadKey;
                meshPayload["blasCacheRequested"] = request.settings.buildBlasCache;
                if (primaryMeshCachePath.empty()) {
                    primaryMeshCachePath = meshPayload.value("cachePath", std::string{});
                }
                fbxCookedMeshesByPayload.emplace(meshPayloadKey, CookedAssetReuseEntry{
                    .guid = meshGuid,
                    .name = meshName,
                    .importedPath = meshPath,
                    .nativePath = nativeMeshPath,
                    .sourceIndex = i,
                });
                cookedPayloads.push_back(meshPayload);
                rootDependencies.push_back(meshGuid);
                (void)writeJson(meshPath, {
                    {"version", 1},
                    {"kind", "ImportedFbxMesh"},
                    {"guid", meshGuid},
                    {"sourcePath", effectiveSourceString},
                    {"originalSourcePath", originalSourceString},
                    {"copiedSourcePath", copiedSourceString},
                    {"sourceHash", sourceHash},
                    {"importSettingsHash", importSettingsHash},
                    {"runtimePayload", meshPayload},
                    {"vertexCount", mesh.vertices.size()},
                    {"indexCount", mesh.indices.size()},
                    {"primitiveCount", mesh.primitives.size()},
                });

                AssetRecord meshRecord;
                meshRecord.guid = meshGuid;
                meshRecord.type = AssetType::Mesh;
                meshRecord.displayName = meshName;
                meshRecord.sourcePath = effectiveSourceString;
                meshRecord.importedPath = genericRelativeOrValue(meshPath, workspace.root);
                meshRecord.cachePath = meshPayload.value("cachePath", std::string{});
                for (const AssetGuid& materialGuid : materialGuids) {
                    if (!materialGuid.empty()) {
                        meshRecord.dependencies.push_back(AssetDependency{materialGuid, "material"});
                    }
                }
                meshRecord.sourceHash = sourceHash;
                meshRecord.importSettingsHash = importSettingsHash;
                meshRecord.lastModifiedTimestamp = timestampString();
                meshRecord.importSettings = request.settings;
                meshRecord.status = AssetImportStatus::Imported;
                records.push_back(std::move(meshRecord));
            }

            setProgress(0.78f, "Writing FBX skeleton and animation metadata");
            skeletalAnimationMetadata = {
                {"schema", "FbxSkeletalAnimationMetadataBridgeV1"},
                {"skeletonCount", fbxData.skeletons.size()},
                {"animationCount", fbxData.animations.size()},
                {"skeletons", fbxData.skeletons},
                {"animations", fbxData.animations},
                {"runtimeSupport", "metadata_bridge_with_skeletal_mesh_binding_runtime_playback_supported"},
            };
            nlohmann::json generatedSkeletalMeshAssets = nlohmann::json::array();
            std::vector<AssetGuid> skeletonGuids;
            std::unordered_map<std::string, AssetGuid> skeletalMeshBindingGuidsByMeshSkin;
            for (size_t i = 0; i < fbxData.skeletons.size(); ++i) {
                const nlohmann::json& skeleton = fbxData.skeletons[i];
                const AssetGuid skeletonGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "FbxSkeleton", i);
                skeletonGuids.push_back(skeletonGuid);
                rootDependencies.push_back(skeletonGuid);
                const std::string authoredSkeletonName = skeleton.value("name", std::string{});
                const std::string skeletonName = safeStem(authoredSkeletonName.empty() ? ("FbxSkeleton_" + std::to_string(i)) : authoredSkeletonName);
                const std::filesystem::path skeletonPath = importedDir / "Skeletons" / (skeletonName + ".rtskeleton.json");
                const std::filesystem::path nativeSkeletonPath = cacheDir / "Skeletons" / (skeletonName + ".rtskeleton");
                std::filesystem::create_directories(skeletonPath.parent_path(), ec);
                std::filesystem::create_directories(nativeSkeletonPath.parent_path(), ec);
                nlohmann::json nativeSkeletonMetadata = skeleton;
                nativeSkeletonMetadata["assetGuid"] = skeletonGuid;
                nativeSkeletonMetadata["assetIndex"] = i;
                nativeSkeletonMetadata["sourceFormat"] = "FBX";
                const NativeAssetCookResult skeletonCook = nativeCooker.cookMetadataPayload(
                    nativeCookInput(skeletonGuid, nativeSkeletonPath, skeletonName),
                    NativeAssetKind::Skeleton,
                    nativeSkeletonMetadata);
                if (!recordNativeCookResult(skeletonCook, skeletonName)) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
                nlohmann::json skeletonPayload = nativeCookRuntimePayloadJson(
                    skeletonCook,
                    NativeAssetKind::Skeleton,
                    skeletonGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                skeletonPayload["assetIndex"] = i;
                skeletonPayload["kind"] = "FbxSkeletonMetadataPayload";
                skeletonPayload["runtimeSupport"] = "metadata_bridge_runtime_skinning_supported";
                cookedPayloads.push_back(skeletonPayload);
                (void)writeJson(skeletonPath, {
                    {"version", 1},
                    {"kind", "ImportedFbxSkeleton"},
                    {"guid", skeletonGuid},
                    {"sourcePath", effectiveSourceString},
                    {"originalSourcePath", originalSourceString},
                    {"copiedSourcePath", copiedSourceString},
                    {"sourceHash", sourceHash},
                    {"importSettingsHash", importSettingsHash},
                    {"runtimePayload", skeletonPayload},
                    {"skeleton", skeleton},
                    {"runtimeSupport", "metadata_bridge_runtime_skinning_supported"},
                });
                generatedSkeletonAssets.push_back({
                    {"guid", skeletonGuid},
                    {"name", skeletonName},
                    {"path", genericRelativeOrValue(skeletonPath, workspace.root)},
                    {"jointCount", skeleton.value("jointCount", 0u)},
                    {"skinnedMeshCount", skeleton.value("skinnedMeshCount", 0u)},
                });

                AssetRecord skeletonRecord;
                skeletonRecord.guid = skeletonGuid;
                skeletonRecord.type = AssetType::Skeleton;
                skeletonRecord.displayName = skeletonName;
                skeletonRecord.sourcePath = effectiveSourceString;
                skeletonRecord.importedPath = genericRelativeOrValue(skeletonPath, workspace.root);
                skeletonRecord.cachePath = skeletonPayload.value("cachePath", std::string{});
                skeletonRecord.thumbnailPath = rootThumbnailPath;
                skeletonRecord.sourceHash = sourceHash;
                skeletonRecord.importSettingsHash = importSettingsHash;
                skeletonRecord.lastModifiedTimestamp = timestampString();
                skeletonRecord.importSettings = request.settings;
                skeletonRecord.status = AssetImportStatus::Imported;
                records.push_back(std::move(skeletonRecord));
            }
            if (!skeletonGuids.empty()) {
                std::unordered_set<std::string> emittedBindings;
                for (size_t nodeIndex = 0; nodeIndex < fbxData.scene.nodes.size(); ++nodeIndex) {
                    const SceneNodeAsset& node = fbxData.scene.nodes[nodeIndex];
                    if (!node.mesh.valid() || node.mesh.index >= meshGuids.size() || node.skinIndex < 0 || static_cast<size_t>(node.skinIndex) >= skeletonGuids.size()) {
                        continue;
                    }
                    const size_t skinIndex = static_cast<size_t>(node.skinIndex);
                    const std::string bindingKey = std::to_string(node.mesh.index) + ":" + std::to_string(skinIndex);
                    if (!emittedBindings.insert(bindingKey).second) {
                        continue;
                    }
                    const nlohmann::json& skeleton = fbxData.skeletons[skinIndex];
                    const AssetGuid meshGuid = meshGuids[node.mesh.index];
                    const AssetGuid skeletonGuid = skeletonGuids[skinIndex];
                    const AssetGuid skeletalMeshGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "FbxSkeletalMesh", generatedSkeletalMeshAssets.size());
                    skeletalMeshBindingGuidsByMeshSkin[bindingKey] = skeletalMeshGuid;
                    rootDependencies.push_back(skeletalMeshGuid);

                    const MeshAsset& mesh = fbxData.assets.meshes()[node.mesh.index];
                    const std::string meshName = mesh.name.empty() ? (node.name.empty() ? ("FbxMesh_" + std::to_string(node.mesh.index)) : node.name) : mesh.name;
                    const std::string bindingName = safeStem(meshName + "_FbxSkin" + std::to_string(skinIndex));
                    const std::filesystem::path bindingPath = importedDir / "Skeletons" / (bindingName + ".rtskeletalmesh.json");
                    const std::filesystem::path nativeBindingPath = cacheDir / "Skeletons" / (bindingName + ".rtskeletalmesh");
                    std::filesystem::create_directories(bindingPath.parent_path(), ec);
                    std::filesystem::create_directories(nativeBindingPath.parent_path(), ec);

                    std::vector<uint32_t> jointRemap;
                    const nlohmann::json skinnedMeshes = skeleton.value("skinnedMeshes", nlohmann::json::array());
                    for (const nlohmann::json& skinnedMesh : skinnedMeshes) {
                        if (skinnedMesh.value("meshIndex", UINT32_MAX) == node.mesh.index && skinnedMesh.contains("bones") && skinnedMesh["bones"].is_array()) {
                            const std::unordered_map<std::string, uint32_t> jointIndex = fbxJointIndexByName(skeleton);
                            for (const nlohmann::json& boneNameJson : skinnedMesh["bones"]) {
                                const std::string boneName = boneNameJson.is_string() ? boneNameJson.get<std::string>() : std::string{};
                                const auto jointIt = jointIndex.find(boneName);
                                if (jointIt != jointIndex.end()) {
                                    jointRemap.push_back(jointIt->second);
                                }
                            }
                            break;
                        }
                    }
                    if (jointRemap.empty()) {
                        const uint32_t jointCount = static_cast<uint32_t>(skeleton.value("jointCount", 0u));
                        jointRemap.reserve(jointCount);
                        for (uint32_t joint = 0; joint < jointCount; ++joint) {
                            jointRemap.push_back(joint);
                        }
                    }
                    if (jointRemap.empty()) {
                        continue;
                    }
                    nlohmann::json bindMetadata = {
                        {"sourceFormat", "FBX"},
                        {"sourceNodeIndex", nodeIndex},
                        {"meshIndex", node.mesh.index},
                        {"skinIndex", skinIndex},
                        {"meshGuid", meshGuid},
                        {"skeletonGuid", skeletonGuid},
                        {"skeletonName", skeleton.value("name", std::string{})},
                        {"jointCount", skeleton.value("jointCount", 0u)},
                        {"jointRemapCount", jointRemap.size()},
                        {"runtimeSupport", "binding_decode_and_mesh_skin_channels_runtime_playback_supported"},
                    };
                    const NativeAssetCookResult bindingCook = nativeCooker.cookSkeletalMeshBinding(
                        nativeCookInput(skeletalMeshGuid, nativeBindingPath, bindingName),
                        meshGuid,
                        skeletonGuid,
                        jointRemap,
                        bindMetadata);
                    if (!recordNativeCookResult(bindingCook, bindingName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json bindingPayload = nativeCookRuntimePayloadJson(
                        bindingCook,
                        NativeAssetKind::SkeletalMesh,
                        skeletalMeshGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    bindingPayload["kind"] = "FbxSkeletalMeshBindingPayload";
                    bindingPayload["meshGuid"] = meshGuid;
                    bindingPayload["skeletonGuid"] = skeletonGuid;
                    bindingPayload["sourceNodeIndex"] = nodeIndex;
                    bindingPayload["meshIndex"] = node.mesh.index;
                    bindingPayload["skinIndex"] = skinIndex;
                    bindingPayload["jointRemapCount"] = jointRemap.size();
                    bindingPayload["runtimeSupport"] = "binding_decode_and_mesh_skin_channels_runtime_playback_supported";
                    cookedPayloads.push_back(bindingPayload);

                    (void)writeJson(bindingPath, {
                        {"version", 1},
                        {"kind", "ImportedFbxSkeletalMeshBinding"},
                        {"guid", skeletalMeshGuid},
                        {"sourcePath", effectiveSourceString},
                        {"originalSourcePath", originalSourceString},
                        {"copiedSourcePath", copiedSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", bindingPayload},
                        {"meshGuid", meshGuid},
                        {"skeletonGuid", skeletonGuid},
                        {"sourceNodeIndex", nodeIndex},
                        {"meshIndex", node.mesh.index},
                        {"skinIndex", skinIndex},
                        {"jointRemap", jointRemap},
                        {"runtimeSupport", "binding_decode_and_mesh_skin_channels_runtime_playback_supported"},
                    });
                    generatedSkeletalMeshAssets.push_back({
                        {"guid", skeletalMeshGuid},
                        {"name", bindingName},
                        {"path", genericRelativeOrValue(bindingPath, workspace.root)},
                        {"meshGuid", meshGuid},
                        {"skeletonGuid", skeletonGuid},
                        {"jointRemapCount", jointRemap.size()},
                    });

                    AssetRecord bindingRecord;
                    bindingRecord.guid = skeletalMeshGuid;
                    bindingRecord.type = AssetType::SkeletalMesh;
                    bindingRecord.displayName = bindingName;
                    bindingRecord.sourcePath = effectiveSourceString;
                    bindingRecord.importedPath = genericRelativeOrValue(bindingPath, workspace.root);
                    bindingRecord.cachePath = bindingPayload.value("cachePath", std::string{});
                    bindingRecord.thumbnailPath = rootThumbnailPath;
                    bindingRecord.dependencies.push_back(AssetDependency{meshGuid, "mesh"});
                    bindingRecord.dependencies.push_back(AssetDependency{skeletonGuid, "skeleton"});
                    bindingRecord.sourceHash = sourceHash;
                    bindingRecord.importSettingsHash = importSettingsHash;
                    bindingRecord.lastModifiedTimestamp = timestampString();
                    bindingRecord.importSettings = request.settings;
                    bindingRecord.status = AssetImportStatus::Imported;
                    records.push_back(std::move(bindingRecord));
                }
            }
            for (size_t i = 0; i < fbxData.animations.size(); ++i) {
                const nlohmann::json& animation = fbxData.animations[i];
                const AssetGuid animationGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "FbxAnimation", i);
                rootDependencies.push_back(animationGuid);
                const std::string authoredAnimationName = animation.value("name", std::string{});
                const std::string animationName = safeStem(authoredAnimationName.empty() ? ("FbxAnimation_" + std::to_string(i)) : authoredAnimationName);
                const std::filesystem::path animationPath = importedDir / "Animations" / (animationName + ".rtanim.json");
                const std::filesystem::path nativeAnimationPath = cacheDir / "Animations" / (animationName + ".rtanim");
                std::filesystem::create_directories(animationPath.parent_path(), ec);
                std::filesystem::create_directories(nativeAnimationPath.parent_path(), ec);
                nlohmann::json nativeAnimationMetadata = animation;
                nativeAnimationMetadata["assetGuid"] = animationGuid;
                nativeAnimationMetadata["assetIndex"] = i;
                nativeAnimationMetadata["sourceFormat"] = "FBX";
                const NativeAssetCookResult animationCook = nativeCooker.cookMetadataPayload(
                    nativeCookInput(animationGuid, nativeAnimationPath, animationName),
                    NativeAssetKind::Animation,
                    nativeAnimationMetadata);
                if (!recordNativeCookResult(animationCook, animationName)) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
                nlohmann::json animationPayload = nativeCookRuntimePayloadJson(
                    animationCook,
                    NativeAssetKind::Animation,
                    animationGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                animationPayload["assetIndex"] = i;
                animationPayload["kind"] = "FbxAnimationMetadataPayload";
                animationPayload["runtimeSupport"] = animation.value("runtimeSupport", std::string("decoded_keyframes_runtime_playback_supported"));
                cookedPayloads.push_back(animationPayload);
                (void)writeJson(animationPath, {
                    {"version", 1},
                    {"kind", "ImportedFbxAnimation"},
                    {"guid", animationGuid},
                    {"sourcePath", effectiveSourceString},
                    {"originalSourcePath", originalSourceString},
                    {"copiedSourcePath", copiedSourceString},
                    {"sourceHash", sourceHash},
                    {"importSettingsHash", importSettingsHash},
                    {"runtimePayload", animationPayload},
                    {"animation", animation},
                    {"runtimeSupport", animation.value("runtimeSupport", std::string("decoded_keyframes_runtime_playback_supported"))},
                });
                generatedAnimationAssets.push_back({
                    {"guid", animationGuid},
                    {"name", animationName},
                    {"path", genericRelativeOrValue(animationPath, workspace.root)},
                    {"channelCount", animation.value("channelCount", 0u)},
                    {"decodedChannelCount", animation.value("decodedChannelCount", 0u)},
                    {"decodedKeyframeCount", animation.value("decodedKeyframeCount", 0u)},
                });

                AssetRecord animationRecord;
                animationRecord.guid = animationGuid;
                animationRecord.type = AssetType::Animation;
                animationRecord.displayName = animationName;
                animationRecord.sourcePath = effectiveSourceString;
                animationRecord.importedPath = genericRelativeOrValue(animationPath, workspace.root);
                animationRecord.cachePath = animationPayload.value("cachePath", std::string{});
                animationRecord.thumbnailPath = rootThumbnailPath;
                animationRecord.sourceHash = sourceHash;
                animationRecord.importSettingsHash = importSettingsHash;
                animationRecord.lastModifiedTimestamp = timestampString();
                animationRecord.importSettings = request.settings;
                animationRecord.status = AssetImportStatus::Imported;
                records.push_back(std::move(animationRecord));
            }

            if (!generatedAnimationAssets.empty()) {
                const nlohmann::json& firstAnimation = generatedAnimationAssets.front();
                const std::string controllerName = safeStem(name + "_Controller");
                const AssetGuid controllerGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "FbxAnimationController", 0);
                const std::filesystem::path controllerPath = importedDir / "AnimationControllers" / (controllerName + ".rtanimcontroller.json");
                const std::filesystem::path nativeControllerPath = cacheDir / "AnimationControllers" / (controllerName + ".rtanimcontroller");
                std::filesystem::create_directories(controllerPath.parent_path(), ec);
                std::filesystem::create_directories(nativeControllerPath.parent_path(), ec);
                const std::string clipGuid = firstAnimation.value("guid", std::string{});
                const std::string clipName = firstAnimation.value("name", std::string("Default"));
                nlohmann::json controllerJson = {
                    {"version", 1},
                    {"kind", "ImportedFbxAnimationController"},
                    {"guid", controllerGuid},
                    {"controller", {
                        {"name", controllerName},
                        {"states", nlohmann::json::array({{
                            {"name", clipName.empty() ? std::string("Default") : clipName},
                            {"clipGuid", clipGuid},
                            {"speed", 1.0f},
                            {"loop", true},
                            {"default", true},
                        }})},
                    }},
                };
                std::vector<std::string> controllerWarnings;
                const AnimationController controller = AnimationController::fromJson(controllerJson, &controllerWarnings);
                if (controller.valid()) {
                    const NativeAssetCookResult controllerCook = nativeCooker.cookAnimationController(
                        nativeCookInput(controllerGuid, nativeControllerPath, controllerName),
                        controller);
                    if (!recordNativeCookResult(controllerCook, controllerName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json controllerPayload = nativeCookRuntimePayloadJson(
                        controllerCook,
                        NativeAssetKind::AnimationController,
                        controllerGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    controllerPayload["kind"] = "FbxAnimationControllerPayload";
                    controllerPayload["clipGuid"] = clipGuid;
                    controllerPayload["animationControllerBindingImplemented"] = true;
                    cookedPayloads.push_back(controllerPayload);
                    controllerJson["sourcePath"] = effectiveSourceString;
                    controllerJson["originalSourcePath"] = originalSourceString;
                    controllerJson["copiedSourcePath"] = copiedSourceString;
                    controllerJson["sourceHash"] = sourceHash;
                    controllerJson["importSettingsHash"] = importSettingsHash;
                    controllerJson["runtimePayload"] = controllerPayload;
                    (void)writeJson(controllerPath, controllerJson);

                    generatedAnimationControllerAssets.push_back({
                        {"guid", controllerGuid},
                        {"name", controllerName},
                        {"path", genericRelativeOrValue(controllerPath, workspace.root)},
                        {"clipGuid", clipGuid},
                        {"stateCount", 1},
                    });

                    AssetRecord controllerRecord;
                    controllerRecord.guid = controllerGuid;
                    controllerRecord.type = AssetType::AnimationController;
                    controllerRecord.displayName = controllerName;
                    controllerRecord.sourcePath = effectiveSourceString;
                    controllerRecord.importedPath = genericRelativeOrValue(controllerPath, workspace.root);
                    controllerRecord.cachePath = controllerPayload.value("cachePath", std::string{});
                    controllerRecord.thumbnailPath = rootThumbnailPath;
                    controllerRecord.sourceHash = sourceHash;
                    controllerRecord.importSettingsHash = importSettingsHash;
                    controllerRecord.lastModifiedTimestamp = timestampString();
                    controllerRecord.importSettings = request.settings;
                    controllerRecord.status = AssetImportStatus::Imported;
                    if (!clipGuid.empty()) {
                        controllerRecord.dependencies.push_back(AssetDependency{clipGuid, "animationClip"});
                    }
                    records.push_back(std::move(controllerRecord));
                } else {
                    result.warnings.push_back("FBX animation controller bridge was skipped because generated controller JSON was invalid.");
                    for (const std::string& warning : controllerWarnings) {
                        result.warnings.push_back(warning);
                    }
                }
            }

            nlohmann::json prefabNodes = nlohmann::json::array();
            for (size_t i = 0; i < fbxData.scene.nodes.size(); ++i) {
                const SceneNodeAsset& node = fbxData.scene.nodes[i];
                nlohmann::json materialGuidsForNode = nlohmann::json::array();
                if (request.settings.importMaterials && node.mesh.valid() && node.mesh.index < meshes.size()) {
                    for (const MeshPrimitiveAsset& primitive : meshes[node.mesh.index].primitives) {
                        if (primitive.material.valid() && primitive.material.index < materialGuids.size()) {
                            materialGuidsForNode.push_back(materialGuids[primitive.material.index]);
                        }
                    }
                }
                std::string skeletalMeshGuidForNode;
                if (node.mesh.valid() && node.skinIndex >= 0) {
                    const std::string bindingKey = std::to_string(node.mesh.index) + ":" + std::to_string(node.skinIndex);
                    const auto bindingIt = skeletalMeshBindingGuidsByMeshSkin.find(bindingKey);
                    if (bindingIt != skeletalMeshBindingGuidsByMeshSkin.end()) {
                        skeletalMeshGuidForNode = bindingIt->second;
                    }
                }
                prefabNodes.push_back({
                    {"index", i},
                    {"sourceNodeIndex", node.sourceNodeIndex},
                    {"name", node.name.empty() ? ("FbxNode_" + std::to_string(i)) : node.name},
                    {"parent", node.parent},
                    {"transform", transformJsonFromMatrix(node.transform)},
                    {"matrix", matrixJson(node.transform)},
                    {"geometricTransform", fbxTransformDiagnostics(nullptr, node.transform)},
                    {"visible", node.visible},
                    {"visibleToCamera", node.visibleToCamera},
                    {"castShadow", node.castShadow},
                    {"receiveShadow", node.receiveShadow},
                    {"renderLayer", node.renderLayer},
                    {"mesh", node.mesh.valid() ? static_cast<int>(node.mesh.index) : -1},
                    {"meshGuid", node.mesh.valid() && node.mesh.index < meshGuids.size() ? meshGuids[node.mesh.index] : std::string{}},
                    {"skinIndex", node.skinIndex},
                    {"skeletalMeshGuid", skeletalMeshGuidForNode},
                    {"materialGuids", materialGuidsForNode},
                    {"hasCamera", node.hasCamera},
                    {"cameraProjection", node.cameraProjection},
                    {"cameraYfov", node.cameraYfov},
                    {"cameraAspectRatio", node.cameraAspectRatio},
                    {"cameraOrthoXmag", node.cameraOrthoXmag},
                    {"cameraOrthoYmag", node.cameraOrthoYmag},
                    {"cameraNear", node.cameraNear},
                    {"cameraFar", node.cameraFar},
                    {"children", node.children},
                });
            }

            const nlohmann::json fbxMaterialDeduplication = {
                {"sourceMaterialCount", materials.size()},
                {"uniqueMaterialCount", fbxCookedMaterialsByContent.size()},
                {"reusedMaterialCount", fbxMaterialContentReuseCount},
                {"persistentCacheHitCount", fbxMaterialPersistentCacheHitCount},
                {"aliases", fbxMaterialDedupAliases},
            };
            const nlohmann::json fbxMeshDeduplication = {
                {"sourceMeshCount", meshes.size()},
                {"uniqueMeshCount", fbxCookedMeshesByPayload.size()},
                {"reusedMeshCount", fbxMeshContentReuseCount},
                {"persistentCacheHitCount", fbxMeshPersistentCacheHitCount},
                {"dedupKeyIncludesMaterialSlots", true},
                {"localBvhCacheRequested", request.settings.buildBlasCache},
                {"aliases", fbxMeshDedupAliases},
            };
            runtimePayload = {
                {"kind", "FbxStaticScenePayload"},
                {"cachePath", primaryMeshCachePath},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"available", !meshGuids.empty()},
                {"validForSource", true},
                {"runtimeStaticMeshCooked", !meshGuids.empty()},
                {"staticMeshImportImplemented", true},
                {"materialCookImplemented", !materialGuids.empty()},
                {"textureCookImplemented", !textureGuids.empty()},
                {"textureBindingImplemented", !textureGuids.empty()},
                {"texturePersistentCacheHitCount", fbxTexturePersistentCacheHitCount},
                {"materialDeduplication", fbxMaterialDeduplication},
                {"meshDeduplication", fbxMeshDeduplication},
                {"skeletonMetadataBridgeImplemented", !generatedSkeletonAssets.empty()},
                {"animationMetadataBridgeImplemented", !generatedAnimationAssets.empty()},
                {"skeletalMeshBindingImplemented", !generatedSkeletalMeshAssets.empty()},
                {"skeletalImportImplemented", !generatedSkeletalMeshAssets.empty()},
                {"animationImportImplemented", !generatedAnimationAssets.empty()},
                {"runtimeAnimationPlaybackImplemented", !generatedAnimationAssets.empty() && !generatedAnimationControllerAssets.empty()},
                {"animationControllerBindingImplemented", !generatedAnimationControllerAssets.empty()},
                {"animationPlaybackDisabledReason", (!generatedAnimationAssets.empty() && !generatedAnimationControllerAssets.empty()) ? "" : "fbx-animation-runtime-playback-requires-decoded-clip-and-controller"},
                {"skeletonAssets", generatedSkeletonAssets},
                {"skeletalMeshAssets", generatedSkeletalMeshAssets},
                {"animationAssets", generatedAnimationAssets},
                {"animationControllerAssets", generatedAnimationControllerAssets},
                {"skeletalAnimationMetadata", skeletalAnimationMetadata},
                {"fbxStaticImport", fbxData.diagnostics},
                {"counts", {
                    {"textures", textureGuids.size()},
                    {"materials", materialGuids.size()},
                    {"meshes", meshGuids.size()},
                    {"skeletons", generatedSkeletonAssets.size()},
                    {"skeletalMeshes", generatedSkeletalMeshAssets.size()},
                    {"animations", generatedAnimationAssets.size()},
                    {"animationControllers", generatedAnimationControllerAssets.size()},
                    {"nodes", fbxData.scene.nodes.size()},
                    {"lights", fbxData.scene.lights.size()},
                }},
            };
            cookedPayloads.push_back(runtimePayload);

            placeholder["runtimePayload"] = runtimePayload;
            placeholder["thumbnail"] = thumbnailMetadata;
            placeholder["fbxMetadata"] = fbxMetadata;
            placeholder["rootNodes"] = fbxData.scene.rootNodes;
            placeholder["nodes"] = prefabNodes;
            placeholder["sourceHierarchy"] = prefabNodes;
            placeholder["nodeCount"] = fbxData.scene.nodes.size();
            placeholder["meshCount"] = meshGuids.size();
            placeholder["materialCount"] = materialGuids.size();
            placeholder["textureCount"] = textureGuids.size();
            placeholder["lightCount"] = fbxData.scene.lights.size();
            placeholder["runtimeStaticMeshCooked"] = !meshGuids.empty();
            placeholder["staticMeshImportImplemented"] = true;
            placeholder["textureCookImplemented"] = !textureGuids.empty();
            placeholder["textureBindingImplemented"] = !textureGuids.empty();
            placeholder["texturePersistentCacheHitCount"] = fbxTexturePersistentCacheHitCount;
            placeholder["materialDeduplication"] = fbxMaterialDeduplication;
            placeholder["meshDeduplication"] = fbxMeshDeduplication;
            placeholder["skeletonMetadataBridgeImplemented"] = !generatedSkeletonAssets.empty();
            placeholder["animationMetadataBridgeImplemented"] = !generatedAnimationAssets.empty();
            placeholder["skeletalMeshBindingImplemented"] = !generatedSkeletalMeshAssets.empty();
            placeholder["skeletalImportImplemented"] = !generatedSkeletalMeshAssets.empty();
            placeholder["animationImportImplemented"] = !generatedAnimationAssets.empty();
            placeholder["runtimeAnimationPlaybackImplemented"] = !generatedAnimationAssets.empty() && !generatedAnimationControllerAssets.empty();
            placeholder["animationControllerBindingImplemented"] = !generatedAnimationControllerAssets.empty();
            placeholder["animationPlaybackDisabledReason"] = (!generatedAnimationAssets.empty() && !generatedAnimationControllerAssets.empty()) ? "" : "fbx-animation-runtime-playback-requires-decoded-clip-and-controller";
            placeholder["skeletalAnimationMetadata"] = skeletalAnimationMetadata;
            placeholder["skeletonAssets"] = generatedSkeletonAssets;
            placeholder["skeletalMeshAssets"] = generatedSkeletalMeshAssets;
            placeholder["animationAssets"] = generatedAnimationAssets;
            placeholder["animationControllerAssets"] = generatedAnimationControllerAssets;
            placeholder["sourceExtension"] = sourceExtension;
            placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
            placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
            cache["runtimePayload"] = runtimePayload;
            cache["cookedPayloads"] = cookedPayloads;
            cache["thumbnail"] = thumbnailMetadata;
            cache["fbxMetadata"] = fbxMetadata;
            cache["textureCount"] = textureGuids.size();
            cache["materialCount"] = materialGuids.size();
            cache["meshCount"] = meshGuids.size();
            cache["texturePersistentCacheHitCount"] = fbxTexturePersistentCacheHitCount;
            cache["materialDeduplication"] = fbxMaterialDeduplication;
            cache["meshDeduplication"] = fbxMeshDeduplication;
            cache["skeletonCount"] = generatedSkeletonAssets.size();
            cache["skeletalMeshCount"] = generatedSkeletalMeshAssets.size();
            cache["animationCount"] = generatedAnimationAssets.size();
            cache["animationControllerCount"] = generatedAnimationControllerAssets.size();
            cache["skeletonAssets"] = generatedSkeletonAssets;
            cache["skeletalMeshAssets"] = generatedSkeletalMeshAssets;
            cache["animationAssets"] = generatedAnimationAssets;
            cache["animationControllerAssets"] = generatedAnimationControllerAssets;
            cache["skeletalAnimationMetadata"] = skeletalAnimationMetadata;
        } else {
            runtimePayload = {
                {"kind", "FbxStaticImportUnavailable"},
                {"sourcePath", effectiveSourceString},
                {"available", false},
                {"staticMeshImportImplemented", false},
                {"disabledReason", "RTV_ENABLE_ASSIMP_IMPORTER=OFF or Assimp unavailable"},
                {"fbxStaticImport", fbxData.diagnostics},
            };
            cookedPayloads.push_back(runtimePayload);
            placeholder["runtimePayload"] = runtimePayload;
            placeholder["thumbnail"] = thumbnailMetadata;
            placeholder["fbxMetadata"] = fbxMetadata;
            cache["runtimePayload"] = runtimePayload;
            cache["cookedPayloads"] = cookedPayloads;
            cache["fbxMetadata"] = fbxMetadata;
        }
    } else if (sourceIsUsd) {
        const auto inspectStart = std::chrono::steady_clock::now();
        setProgress(0.42f, "Parsing USD stage metadata");
        traceImport("USD import: load stage metadata");
        UsdStageImportData usdData = loadUsdStageMetadata(effectiveSourcePath);
        traceImport("USD import: inspect USDZ textures");
        UsdzPackageTextureData usdzPackageTextures = inspectUsdzPackageTextures(effectiveSourcePath);
        UsdRuntimeMeshCookData usdRuntimeMeshes;
        UsdSceneEntityImportData usdSceneEntities;
        for (const std::string& warning : usdData.warnings) {
            result.warnings.push_back(warning);
        }
        for (const std::string& warning : usdzPackageTextures.warnings) {
            result.warnings.push_back(warning);
        }
        for (const std::string& error : usdzPackageTextures.errors) {
            result.warnings.push_back(error);
        }
        usdMetadata = usdData.diagnostics;
        usdMetadata["usdzPackageTextures"] = usdzPackageTextures.diagnostics;
        usdMetadata["usdzTextureProvenanceInspectionImplemented"] = usdzPackageTextures.inspected;
        usdMetadata["usdzPackagedTextureEntryCount"] = usdzPackageTextures.textureEntryCount;
        result.workerInspectMs = elapsedMilliseconds(inspectStart);
        rootThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(effectiveSourcePath, workspace.root) : std::string{};
        thumbnailMetadata = thumbnailMetadataJson("GeneratedSourcePreview", rootThumbnailPath, sourceHash, importSettingsHash);

        nlohmann::json usdzTextureAssets = nlohmann::json::array();
        std::vector<AssetGuid> usdzTextureGuids;
        std::unordered_map<std::string, AssetGuid> usdzTextureGuidByLookupKey;
        std::unordered_map<std::string, nlohmann::json> usdzTextureAssetByLookupKey;
        bool usdzTextureExtractionImplemented = false;
        bool usdzTextureNativeCookImplemented = false;
        size_t usdzExtractedTextureCount = 0;
        size_t usdzNativeTextureCookCount = 0;
        size_t usdzTexturePersistentCacheHitCount = 0;
        nlohmann::json usdExternalTextureAssets = nlohmann::json::array();
        std::vector<AssetGuid> usdExternalTextureGuids;
        std::unordered_set<std::string> usdExternalTextureCookKeys;
        bool usdExternalTextureNativeCookImplemented = false;
        bool usdExternalTextureCookFailed = false;
        if (request.settings.importTextures && usdzPackageTextures.isUsdz && !usdzPackageTextures.entries.empty()) {
            setProgress(0.50f, "Extracting USDZ packaged textures");
            size_t textureIndex = 0;
            for (const UsdzPackageEntry& entry : usdzPackageTextures.entries) {
                if (!entry.isTexture) {
                    continue;
                }
                std::string extractionError;
                std::optional<std::vector<uint8_t>> bytes = extractUsdzEntryBytes(effectiveSourcePath, entry, extractionError);
                if (!bytes.has_value()) {
                    result.warnings.push_back("Skipped USDZ packaged texture '" + entry.path + "': " + extractionError + ".");
                    ++textureIndex;
                    continue;
                }
                const std::string textureName = safeStem(std::filesystem::path(entry.path).stem().string().empty()
                    ? ("UsdzTexture_" + std::to_string(textureIndex))
                    : std::filesystem::path(entry.path).stem().string()) + "_" + std::to_string(textureIndex);
                const std::filesystem::path extractedPath = cacheDir / "USDZ" / "ExtractedTextures" / (textureName + std::filesystem::path(entry.path).extension().string());
                const std::filesystem::path texturePath = importedDir / "Textures" / (textureName + ".rttexture.json");
                const std::filesystem::path nativeTexturePath = cacheDir / "USDZ" / "Textures" / (textureName + ".rttexture");
                std::filesystem::create_directories(extractedPath.parent_path(), ec);
                std::filesystem::create_directories(texturePath.parent_path(), ec);
                std::filesystem::create_directories(nativeTexturePath.parent_path(), ec);
                {
                    std::ofstream extracted(extractedPath, std::ios::binary);
                    if (!extracted.is_open()) {
                        result.warnings.push_back("Could not write extracted USDZ packaged texture: " + extractedPath.string());
                        ++textureIndex;
                        continue;
                    }
                    extracted.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
                    result.generatedFiles.push_back(extractedPath);
                }
                ++usdzExtractedTextureCount;
                usdzTextureExtractionImplemented = true;

                const nlohmann::json textureRole = inferTextureRole(std::filesystem::path(entry.path), AssetType::Texture);
                const std::string roleName = textureRole.value("role", std::string("unknown"));
                const NativeTextureRole role = nativeTextureRoleFromString(roleName);
                const NativeTextureColorSpace colorSpace = colorSpaceForTextureRole(role);
                try {
                    const TextureData textureData = TextureLoader::load(
                        bytes->data(),
                        bytes->size(),
                        importTextureFormatSupport,
                        role,
                        colorSpace);
                    TextureAsset texture = textureAssetFromData(
                        textureData,
                        textureName,
                        extractedPath,
                        colorSpace);
                    const AssetGuid textureGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "UsdzTexture", textureIndex);
                    usdzTextureGuids.push_back(textureGuid);
                    rootDependencies.push_back(textureGuid);
                    const std::optional<NativeAssetCookResult> cachedTextureCook = reusableNativeCookResult(
                        nativeTexturePath,
                        NativeAssetKind::Texture,
                        textureGuid,
                        effectiveSourcePath);
                    const bool texturePersistentCacheReused = cachedTextureCook.has_value();
                    if (texturePersistentCacheReused) {
                        ++usdzTexturePersistentCacheHitCount;
                    }
                    const NativeAssetCookResult textureCook = texturePersistentCacheReused
                        ? *cachedTextureCook
                        : nativeCooker.cookTexture(
                            nativeCookInput(textureGuid, nativeTexturePath, textureName),
                            texture,
                            roleName);
                    if (!recordNativeCookResult(textureCook, textureName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json texturePayload = nativeCookRuntimePayloadJson(
                        textureCook,
                        NativeAssetKind::Texture,
                        textureGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    texturePayload["kind"] = "UsdzPackagedTexturePayload";
                    texturePayload["sourcePackagePath"] = effectiveSourceString;
                    texturePayload["sourcePackageEntry"] = entry.path;
                    texturePayload["extractedPath"] = genericRelativeOrValue(extractedPath, workspace.root);
                    texturePayload["textureExtractionImplemented"] = true;
                    texturePayload["textureNativeCookImplemented"] = true;
                    texturePayload["textureRole"] = textureRole;
                    texturePayload["persistentCacheReused"] = texturePersistentCacheReused;
                    cookedPayloads.push_back(texturePayload);
                    ++usdzNativeTextureCookCount;
                    usdzTextureNativeCookImplemented = true;

                    (void)writeJson(texturePath, {
                        {"version", 1},
                        {"kind", "ImportedUsdzTexture"},
                        {"guid", textureGuid},
                        {"sourcePath", effectiveSourceString},
                        {"sourcePackageEntry", entry.path},
                        {"extractedPath", genericRelativeOrValue(extractedPath, workspace.root)},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", texturePayload},
                        {"width", texture.width},
                        {"height", texture.height},
                        {"channels", texture.channels},
                        {"colorSpace", textureColorSpaceLabel(texture)},
                        {"textureRole", textureRole},
                        {"usdzPackageTexture", {
                            {"textureExtractionImplemented", true},
                            {"textureNativeCookImplemented", true},
                            {"persistentCacheReused", texturePersistentCacheReused},
                            {"compressionMethod", entry.compressionMethod},
                            {"compressedSize", entry.compressedSize},
                            {"uncompressedSize", entry.uncompressedSize},
                        }},
                    });
                    AssetRecord textureRecord;
                    textureRecord.guid = textureGuid;
                    textureRecord.type = AssetType::Texture;
                    textureRecord.displayName = textureName;
                    textureRecord.sourcePath = effectiveSourceString;
                    textureRecord.importedPath = genericRelativeOrValue(texturePath, workspace.root);
                    textureRecord.cachePath = texturePayload.value("cachePath", genericRelativeOrValue(nativeTexturePath, workspace.root));
                    textureRecord.thumbnailPath = request.settings.generateThumbnails ? genericRelativeOrValue(extractedPath, workspace.root) : std::string{};
                    textureRecord.sourceHash = sourceHash;
                    textureRecord.importSettingsHash = importSettingsHash;
                    textureRecord.lastModifiedTimestamp = timestampString();
                    textureRecord.importSettings = request.settings;
                    textureRecord.status = AssetImportStatus::Imported;
                    records.push_back(std::move(textureRecord));
                    usdzTextureAssets.push_back({
                        {"guid", textureGuid},
                        {"name", textureName},
                        {"path", genericRelativeOrValue(texturePath, workspace.root)},
                        {"cachePath", texturePayload.value("cachePath", std::string{})},
                        {"sourcePackageEntry", entry.path},
                        {"extractedPath", genericRelativeOrValue(extractedPath, workspace.root)},
                        {"textureRole", textureRole},
                        {"persistentCacheReused", texturePersistentCacheReused},
                        {"width", texture.width},
                        {"height", texture.height},
                    });
                    const nlohmann::json textureAssetSummary = usdzTextureAssets.back();
                    for (const std::string& lookupKey : usdTextureLookupKeys(entry.path)) {
                        usdzTextureGuidByLookupKey[lookupKey] = textureGuid;
                        usdzTextureAssetByLookupKey[lookupKey] = textureAssetSummary;
                    }
                } catch (const std::exception& ex) {
                    result.warnings.push_back("Could not decode extracted USDZ packaged texture '" + entry.path + "': " + ex.what());
                }
                ++textureIndex;
            }
        }
        usdMetadata["usdzTextureExtractionImplemented"] = usdzTextureExtractionImplemented;
        usdMetadata["usdzTextureNativeCookImplemented"] = usdzTextureNativeCookImplemented;
        usdMetadata["usdzExtractedTextureCount"] = usdzExtractedTextureCount;
        usdMetadata["usdzNativeTextureCookCount"] = usdzNativeTextureCookCount;
        usdMetadata["usdzTexturePersistentCacheHitCount"] = usdzTexturePersistentCacheHitCount;
        usdMetadata["usdzTextureAssets"] = usdzTextureAssets;
        float usdMaterialCookProgressValue = 0.70f;
        size_t usdExternalTextureCookProgressCount = 0;
        size_t usdExternalTextureSingleFlightReuseCount = 0;
        size_t usdExternalTexturePersistentCacheHitCount = 0;
        auto cookUsdExternalTexture = [&](const nlohmann::json& reference, NativeTextureRole referenceRole) -> bool {
            if (!request.settings.importTextures || usdExternalTextureCookFailed) {
                return false;
            }
            const std::string assetPath = reference.value("assetPath", std::string{});
            const std::string resolvedPath = reference.value("resolvedPath", std::string{});
            const std::optional<std::filesystem::path> textureSourcePath = resolveUsdExternalTexturePath(effectiveSourcePath, assetPath, resolvedPath);
            if (!textureSourcePath.has_value()) {
                return false;
            }
            NativeTextureRole nativeRole = referenceRole;
            nlohmann::json textureRole = nlohmann::json::object();
            if (nativeRole == NativeTextureRole::Unknown) {
                textureRole = inferTextureRole(*textureSourcePath, AssetType::Texture);
                nativeRole = nativeTextureRoleFromString(textureRole.value("role", std::string("unknown")));
            }
            if (textureRole.empty()) {
                textureRole = {
                    {"role", nativeTextureRoleName(nativeRole)},
                    {"source", "usdShaderTextureReference"},
                    {"shaderPath", reference.value("shaderPath", std::string{})},
                    {"attribute", reference.value("attribute", std::string{})},
                    {"confidence", nativeRole == NativeTextureRole::Unknown ? "unknown" : "authored-slot"},
                };
            }
            const NativeTextureColorSpace colorSpace = colorSpaceForTextureRole(nativeRole);
            const std::string cookKey = nativeTextureRoleName(nativeRole) + std::string(":") + lowerString(textureSourcePath->lexically_normal().generic_string());
            if (usdExternalTextureCookKeys.find(cookKey) != usdExternalTextureCookKeys.end()) {
                ++usdExternalTextureSingleFlightReuseCount;
                return true;
            }

            ++usdExternalTextureCookProgressCount;
            setProgress(
                usdMaterialCookProgressValue,
                "Cooking USD external texture " +
                    std::to_string(usdExternalTextureCookProgressCount) +
                    ": " +
                    textureSourcePath->filename().string());

            TextureData textureData;
            try {
                textureData = TextureLoader::load(textureSourcePath->string(), importTextureFormatSupport, nativeRole, colorSpace);
            } catch (const std::exception& ex) {
                result.warnings.push_back("USD external texture decode failed for '" + textureSourcePath->string() + "': " + ex.what());
                return false;
            }

            const size_t textureIndex = usdExternalTextureGuids.size();
            const AssetGuid textureGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "UsdExternalTexture", textureIndex);
            const std::string textureName = safeStem(textureSourcePath->stem().string() + "_" + nativeTextureRoleName(nativeRole) + "_" + std::to_string(textureIndex));
            const std::filesystem::path texturePath = importedDir / "Textures" / (textureName + ".rttexture.json");
            const std::filesystem::path nativeTexturePath = cacheDir / "USD" / "Textures" / (textureName + ".rttexture");
            std::filesystem::create_directories(texturePath.parent_path(), ec);
            std::filesystem::create_directories(nativeTexturePath.parent_path(), ec);

            TextureAsset textureAsset = textureAssetFromData(textureData, textureName, *textureSourcePath, colorSpace);
            const std::optional<NativeAssetCookResult> cachedTextureCook = reusableNativeCookResult(
                nativeTexturePath,
                NativeAssetKind::Texture,
                textureGuid,
                *textureSourcePath);
            const bool texturePersistentCacheReused = cachedTextureCook.has_value();
            if (texturePersistentCacheReused) {
                ++usdExternalTexturePersistentCacheHitCount;
            }
            const NativeAssetCookResult textureCook = texturePersistentCacheReused
                ? *cachedTextureCook
                : nativeCooker.cookTexture(
                    nativeCookInput(textureGuid, nativeTexturePath, textureName),
                    textureAsset,
                    nativeTextureRoleName(nativeRole));
            if (!recordNativeCookResult(textureCook, textureName)) {
                usdExternalTextureCookFailed = true;
                return false;
            }

            nlohmann::json texturePayload = nativeCookRuntimePayloadJson(
                textureCook,
                NativeAssetKind::Texture,
                textureGuid,
                workspace.root,
                effectiveSourcePath,
                sourceHash,
                importSettingsHash);
            texturePayload["kind"] = "UsdExternalTexturePayload";
            texturePayload["sourceTexturePath"] = textureSourcePath->generic_string();
            texturePayload["sourceUsdAssetPath"] = assetPath;
            texturePayload["sourceUsdResolvedPath"] = resolvedPath;
            texturePayload["textureNativeCookImplemented"] = true;
            texturePayload["textureRole"] = textureRole;
            texturePayload["singleFlightKey"] = cookKey;
            texturePayload["persistentCacheReused"] = texturePersistentCacheReused;
            cookedPayloads.push_back(texturePayload);
            rootDependencies.push_back(textureGuid);
            usdExternalTextureGuids.push_back(textureGuid);
            usdExternalTextureCookKeys.insert(cookKey);
            usdExternalTextureNativeCookImplemented = true;

            (void)writeJson(texturePath, {
                {"version", 1},
                {"kind", "ImportedUsdExternalTexture"},
                {"guid", textureGuid},
                {"sourcePath", textureSourcePath->generic_string()},
                {"rootSourcePath", effectiveSourceString},
                {"sourceUsdAssetPath", assetPath},
                {"sourceUsdResolvedPath", resolvedPath},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"runtimePayload", texturePayload},
                {"width", textureAsset.width},
                {"height", textureAsset.height},
                {"channels", textureAsset.channels},
                {"colorSpace", textureColorSpaceLabel(textureAsset)},
                {"textureRole", textureRole},
                {"usdShaderTextureReference", {
                    {"shaderPath", reference.value("shaderPath", std::string{})},
                    {"shaderId", reference.value("shaderId", std::string{})},
                    {"attribute", reference.value("attribute", std::string{})},
                }},
            });

            AssetRecord textureRecord;
            textureRecord.guid = textureGuid;
            textureRecord.type = AssetType::Texture;
            textureRecord.displayName = textureName;
            textureRecord.sourcePath = textureSourcePath->generic_string();
            textureRecord.importedPath = genericRelativeOrValue(texturePath, workspace.root);
            textureRecord.cachePath = texturePayload.value("cachePath", genericRelativeOrValue(nativeTexturePath, workspace.root));
            textureRecord.thumbnailPath = request.settings.generateThumbnails ? genericRelativeOrValue(*textureSourcePath, workspace.root) : std::string{};
            textureRecord.sourceHash = sourceHash;
            textureRecord.importSettingsHash = importSettingsHash;
            textureRecord.lastModifiedTimestamp = timestampString();
            textureRecord.importSettings = request.settings;
            textureRecord.status = AssetImportStatus::Imported;
            records.push_back(std::move(textureRecord));

            usdExternalTextureAssets.push_back({
                {"guid", textureGuid},
                {"name", textureName},
                {"path", genericRelativeOrValue(texturePath, workspace.root)},
                {"cachePath", texturePayload.value("cachePath", std::string{})},
                {"sourcePath", textureSourcePath->generic_string()},
                {"sourceUsdAssetPath", assetPath},
                {"sourceUsdResolvedPath", resolvedPath},
                {"textureRole", textureRole},
                {"singleFlightKey", cookKey},
                {"persistentCacheReused", texturePersistentCacheReused},
                {"width", textureAsset.width},
                {"height", textureAsset.height},
            });
            const nlohmann::json textureAssetSummary = usdExternalTextureAssets.back();
            for (const std::string& lookupPath : {assetPath, resolvedPath, textureSourcePath->generic_string(), textureSourcePath->filename().generic_string()}) {
                for (const std::string& lookupKey : usdTextureLookupKeys(lookupPath)) {
                    usdzTextureGuidByLookupKey[lookupKey] = textureGuid;
                    usdzTextureAssetByLookupKey[lookupKey] = textureAssetSummary;
                }
            }
            return true;
        };

        if (usdData.supported && usdData.errors.empty()) {
            setProgress(0.58f, "Cooking USD native meshes");
            traceImport("USD import: load runtime meshes");
            usdRuntimeMeshes = loadUsdRuntimeMeshes(effectiveSourcePath, name);
            for (const std::string& warning : usdRuntimeMeshes.warnings) {
                result.warnings.push_back(warning);
            }
            if (!usdRuntimeMeshes.errors.empty()) {
                for (const std::string& error : usdRuntimeMeshes.errors) {
                    result.warnings.push_back(error);
                }
            }
            setProgress(0.64f, "Preparing USD cameras and lights");
            traceImport("USD import: reuse metadata scene entities");
            usdSceneEntities = usdSceneEntitiesFromStageMetadata(effectiveSourcePath, usdData);
            for (const std::string& warning : usdSceneEntities.warnings) {
                result.warnings.push_back(warning);
            }
            if (!usdSceneEntities.errors.empty()) {
                for (const std::string& error : usdSceneEntities.errors) {
                    result.warnings.push_back(error);
                }
            }

            nlohmann::json usdMeshAssets = nlohmann::json::array();
            nlohmann::json usdMaterialAssets = nlohmann::json::array();
            nlohmann::json generatedUsdAnimationAssets = nlohmann::json::array();
            nlohmann::json generatedUsdAnimationControllerAssets = nlohmann::json::array();
            std::vector<AssetGuid> usdMeshGuids;
            std::vector<AssetGuid> usdMaterialGuids;
            std::unordered_map<std::string, uint32_t> usdMaterialSlotByPath;
            nlohmann::json usdMaterialDeduplication = {
                {"enabled", request.settings.importMaterials && usdRuntimeMeshes.supported},
                {"key", "native_material_content_texture_guid_slots"},
                {"sourceMaterialCount", 0},
                {"sourceNetworkReusedCount", 0},
                {"uniqueMaterialCount", 0},
                {"reusedMaterialCount", 0},
                {"persistentCacheHitCount", 0},
                {"aliases", nlohmann::json::array()},
            };
            std::string primaryUsdNativeCachePath;
            bool usdShaderNetworkConverted = false;
            bool usdTextureReferenceExtractionImplemented = false;
            bool usdShaderTextureMaterialBindingImplemented = false;
            size_t usdTextureReferenceCount = 0;
            size_t usdShaderTextureBindingCount = 0;
            size_t usdUnsupportedShaderNodeCount = 0;
            nlohmann::json usdUnsupportedShaderNodes = nlohmann::json::array();
            std::vector<std::string> usdMaterialPathsForCook;
            std::unordered_set<std::string> usdMaterialPathsSeen;
            auto appendUsdMaterialPath = [&](const std::string& materialPath) {
                if (materialPath.empty() || usdMaterialPathsSeen.find(materialPath) != usdMaterialPathsSeen.end()) {
                    return;
                }
                usdMaterialPathsSeen.insert(materialPath);
                usdMaterialPathsForCook.push_back(materialPath);
            };
            for (const std::string& materialPath : usdRuntimeMeshes.materialBindingPaths) {
                appendUsdMaterialPath(materialPath);
            }
            for (const std::vector<std::string>& primitiveMaterialPaths : usdRuntimeMeshes.primitiveMaterialBindingPaths) {
                for (const std::string& materialPath : primitiveMaterialPaths) {
                    appendUsdMaterialPath(materialPath);
                }
            }
            if (request.settings.importMaterials && usdRuntimeMeshes.supported && !usdMaterialPathsForCook.empty()) {
                std::vector<UsdMaterialShaderNetworkRequest> usdMaterialShaderNetworkRequests;
                usdMaterialShaderNetworkRequests.reserve(usdMaterialPathsForCook.size());
                for (size_t materialIndex = 0; materialIndex < usdMaterialPathsForCook.size(); ++materialIndex) {
                    const std::string& materialPath = usdMaterialPathsForCook[materialIndex];
                    if (materialPath.empty()) {
                        continue;
                    }
                    usdMaterialShaderNetworkRequests.push_back({
                        materialPath,
                        usdMaterialNameFromPath(materialPath, "UsdMaterial") + "_" + std::to_string(usdMaterialShaderNetworkRequests.size()),
                    });
                }
                const size_t usdMaterialShaderNetworkCount = usdMaterialShaderNetworkRequests.size();
                traceImport("USD import: load material shader networks");
                const std::vector<UsdMaterialShaderNetworkData> usdMaterialShaderNetworks = loadUsdMaterialShaderNetworks(
                    effectiveSourcePath,
                    usdMaterialShaderNetworkRequests,
                    [&](size_t materialIndex, size_t materialCount, const std::string&) {
                        if (materialCount == 0u) {
                            return;
                        }
                        const size_t completed = std::min(materialIndex + 1u, materialCount);
                        const float t = static_cast<float>(completed) / static_cast<float>(materialCount);
                        setProgress(
                            0.66f + 0.04f * t,
                            "Reading USD material shaders " + std::to_string(completed) + " / " + std::to_string(materialCount));
                    });
                struct UsdCookedMaterialReuseEntry {
                    AssetGuid guid;
                    std::string name;
                    std::string path;
                    std::string cachePath;
                    std::string sourceMaterialPath;
                    uint32_t slot = UINT32_MAX;
                    size_t requestIndex = 0;
                    size_t uniqueMaterialIndex = 0;
                };
                std::unordered_map<std::string, UsdCookedMaterialReuseEntry> usdCookedMaterialByContentKey;
                nlohmann::json usdMaterialContentReuseAliases = nlohmann::json::array();
                size_t usdUniqueMaterialCookCount = 0;
                size_t usdReusedMaterialCookCount = 0;
                size_t usdMaterialSourceNetworkReuseCount = 0;
                size_t usdMaterialPersistentCacheHitCount = 0;
                setProgress(
                    0.70f,
                    "Cooking unique USD materials 0 / " + std::to_string(usdMaterialShaderNetworkCount));
                for (size_t materialRequestIndex = 0; materialRequestIndex < usdMaterialShaderNetworkRequests.size(); ++materialRequestIndex) {
                    const UsdMaterialShaderNetworkRequest& materialRequest = usdMaterialShaderNetworkRequests[materialRequestIndex];
                    const std::string& materialPath = materialRequest.materialPath;
                    if (materialPath.empty() || usdMaterialSlotByPath.find(materialPath) != usdMaterialSlotByPath.end()) {
                        continue;
                    }
                    const std::string materialName = materialRequest.materialName;
                    const float materialCookProgress = usdMaterialShaderNetworkCount > 0u
                        ? static_cast<float>(materialRequestIndex + 1u) / static_cast<float>(usdMaterialShaderNetworkCount)
                        : 1.0f;
                    usdMaterialCookProgressValue = 0.70f + 0.06f * materialCookProgress;
                    setProgress(
                        usdMaterialCookProgressValue,
                        "Cooking unique USD materials " + std::to_string(usdUniqueMaterialCookCount) + " / " + std::to_string(usdMaterialShaderNetworkCount) +
                            " (reused " + std::to_string(usdReusedMaterialCookCount) + ")");
                    traceImport("USD import: resolve material asset " + materialPath);
                    const UsdMaterialShaderNetworkData shaderNetwork = materialRequestIndex < usdMaterialShaderNetworks.size()
                        ? usdMaterialShaderNetworks[materialRequestIndex]
                        : loadUsdMaterialShaderNetwork(effectiveSourcePath, materialPath, materialName);
                    const bool sourceNetworkDeduplicated = shaderNetwork.diagnostics.value("sourceNetworkDeduplicated", false);
                    if (sourceNetworkDeduplicated) {
                        ++usdMaterialSourceNetworkReuseCount;
                    }
                    MaterialAsset material = shaderNetwork.material;
                    usdShaderNetworkConverted = usdShaderNetworkConverted || shaderNetwork.shaderNetworkConverted;
                    usdTextureReferenceExtractionImplemented = usdTextureReferenceExtractionImplemented || shaderNetwork.diagnostics.value("textureReferenceExtractionImplemented", false);
                    usdTextureReferenceCount += shaderNetwork.diagnostics.value("textureReferenceCount", 0u);
                    const nlohmann::json materialUnsupportedShaderNodes = shaderNetwork.diagnostics.value("unsupportedShaderNodes", nlohmann::json::array());
                    if (materialUnsupportedShaderNodes.is_array()) {
                        usdUnsupportedShaderNodeCount += materialUnsupportedShaderNodes.size();
                        for (const nlohmann::json& unsupportedShaderNode : materialUnsupportedShaderNodes) {
                            usdUnsupportedShaderNodes.push_back(unsupportedShaderNode);
                        }
                    }
                    std::vector<AssetGuid> materialTextureGuids;
                    std::unordered_map<std::string, uint32_t> materialTextureSlotByGuid;
                    nlohmann::json usdShaderTextureBindings = nlohmann::json::array();
                    size_t materialTextureBindingCount = 0;
                    const nlohmann::json textureReferences = shaderNetwork.diagnostics.value("textureReferences", nlohmann::json::array());
                    if (textureReferences.is_array()) {
                        for (const nlohmann::json& reference : textureReferences) {
                            if (!reference.is_object()) {
                                continue;
                            }
                            const std::string assetPath = reference.value("assetPath", std::string{});
                            const std::string resolvedPath = reference.value("resolvedPath", std::string{});
                            AssetGuid matchedGuid;
                            nlohmann::json matchedTextureAsset = nlohmann::json::object();
                            auto findTexture = [&](const std::string& path) {
                                for (const std::string& lookupKey : usdTextureLookupKeys(path)) {
                                    const auto guidIt = usdzTextureGuidByLookupKey.find(lookupKey);
                                    if (guidIt == usdzTextureGuidByLookupKey.end()) {
                                        continue;
                                    }
                                    matchedGuid = guidIt->second;
                                    const auto assetIt = usdzTextureAssetByLookupKey.find(lookupKey);
                                    if (assetIt != usdzTextureAssetByLookupKey.end()) {
                                        matchedTextureAsset = assetIt->second;
                                    }
                                    return true;
                                }
                                return false;
                            };
                            const NativeTextureRole role = usdTextureRoleForReference(reference);
                            bool matchedTexture = findTexture(assetPath) || findTexture(resolvedPath);
                            if (!matchedTexture && cookUsdExternalTexture(reference, role)) {
                                matchedTexture = findTexture(assetPath) || findTexture(resolvedPath);
                            }
                            if (usdExternalTextureCookFailed) {
                                result.workerTotalMs = elapsedMilliseconds(workerStart);
                                return result;
                            }
                            const std::string slotName = usdMaterialTextureSlotName(role);
                            bool bound = false;
                            uint32_t materialTextureSlot = UINT32_MAX;
                            if (matchedTexture && role != NativeTextureRole::Unknown) {
                                const auto slotIt = materialTextureSlotByGuid.find(matchedGuid);
                                if (slotIt != materialTextureSlotByGuid.end()) {
                                    materialTextureSlot = slotIt->second;
                                } else {
                                    materialTextureSlot = static_cast<uint32_t>(materialTextureGuids.size());
                                    materialTextureSlotByGuid.emplace(matchedGuid, materialTextureSlot);
                                    materialTextureGuids.push_back(matchedGuid);
                                }
                                bound = bindUsdMaterialTextureSlot(material, role, materialTextureSlot);
                            }
                            if (bound) {
                                ++materialTextureBindingCount;
                                ++usdShaderTextureBindingCount;
                                usdShaderTextureMaterialBindingImplemented = true;
                            }
                            usdShaderTextureBindings.push_back({
                                {"assetPath", assetPath},
                                {"resolvedPath", resolvedPath},
                                {"shaderPath", reference.value("shaderPath", std::string{})},
                                {"shaderId", reference.value("shaderId", std::string{})},
                                {"attribute", reference.value("attribute", std::string{})},
                                {"matchedTexture", matchedTexture},
                                {"matchedTextureGuid", matchedGuid},
                                {"matchedTextureAsset", matchedTextureAsset},
                                {"role", nativeTextureRoleName(role)},
                                {"slot", slotName},
                                {"materialTextureSlot", materialTextureSlot},
                                {"bound", bound},
                                {"reason", bound ? "bound_to_native_material_texture_slot" : matchedTexture ? "unsupported_or_unknown_texture_role" : "no_matching_extracted_usdz_texture"},
                            });
                        }
                    }
                    const std::string materialKey = materialContentDedupKey(material, materialTextureGuids);
                    if (const auto sharedIt = usdCookedMaterialByContentKey.find(materialKey); sharedIt != usdCookedMaterialByContentKey.end()) {
                        const UsdCookedMaterialReuseEntry& shared = sharedIt->second;
                        usdMaterialSlotByPath.emplace(materialPath, shared.slot);
                        ++usdReusedMaterialCookCount;
                        nlohmann::json alias = {
                            {"guid", shared.guid},
                            {"name", shared.name},
                            {"aliasName", materialName},
                            {"path", shared.path},
                            {"cachePath", shared.cachePath},
                            {"sourceMaterialPath", materialPath},
                            {"shaderNetworkConversionImplemented", shaderNetwork.shaderNetworkConverted},
                            {"textureReferenceExtractionImplemented", shaderNetwork.diagnostics.value("textureReferenceExtractionImplemented", false)},
                            {"textureReferenceCount", shaderNetwork.diagnostics.value("textureReferenceCount", 0u)},
                            {"unsupportedShaderNodeCount", materialUnsupportedShaderNodes.is_array() ? materialUnsupportedShaderNodes.size() : 0u},
                            {"unsupportedShaderNodes", materialUnsupportedShaderNodes},
                            {"shaderTextureMaterialBindingImplemented", materialTextureBindingCount > 0},
                            {"shaderTextureBindingCount", materialTextureBindingCount},
                            {"usdShaderTextureBindings", usdShaderTextureBindings},
                            {"usdShaderNetwork", shaderNetwork.diagnostics},
                            {"textureNativeCookImplemented", !materialTextureGuids.empty()},
                            {"sourceNetworkDeduplicated", sourceNetworkDeduplicated},
                            {"materialDeduplicated", true},
                            {"sharedMaterialKey", materialKey},
                            {"sharedSourceMaterialPath", shared.sourceMaterialPath},
                            {"sharedMaterialSlot", shared.slot},
                            {"sharedRequestIndex", shared.requestIndex},
                            {"uniqueMaterialIndex", shared.uniqueMaterialIndex},
                        };
                        usdMaterialContentReuseAliases.push_back(alias);
                        usdMaterialAssets.push_back(std::move(alias));
                        continue;
                    }

                    const uint32_t materialSlot = static_cast<uint32_t>(usdMaterialGuids.size());
                    usdMaterialSlotByPath.emplace(materialPath, materialSlot);
                    const AssetGuid materialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "UsdMaterial", materialRequestIndex);
                    rootDependencies.push_back(materialGuid);
                    usdMaterialGuids.push_back(materialGuid);
                    const size_t uniqueMaterialIndex = usdUniqueMaterialCookCount++;
                    const std::filesystem::path materialPathJson = importedDir / "Materials" / (materialName + ".rtmaterial.json");
                    const std::filesystem::path materialCache = cacheDir / "Materials" / (materialName + ".rtmaterial");
                    std::filesystem::create_directories(materialPathJson.parent_path(), ec);
                    std::filesystem::create_directories(materialCache.parent_path(), ec);

                    traceImport("USD import: cook material asset " + materialPath);
                    const std::optional<NativeAssetCookResult> cachedMaterialCook = reusableNativeCookResult(
                        materialCache,
                        NativeAssetKind::Material,
                        materialGuid,
                        effectiveSourcePath);
                    const bool materialPersistentCacheReused = cachedMaterialCook.has_value();
                    if (materialPersistentCacheReused) {
                        ++usdMaterialPersistentCacheHitCount;
                    }
                    const NativeAssetCookResult materialCook = materialPersistentCacheReused
                        ? *cachedMaterialCook
                        : nativeCooker.cookMaterial(
                            nativeCookInput(materialGuid, materialCache, materialName),
                            material,
                            materialTextureGuids);
                    if (!recordNativeCookResult(materialCook, materialName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json materialPayload = nativeCookRuntimePayloadJson(
                        materialCook,
                        NativeAssetKind::Material,
                        materialGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    if (primaryUsdNativeCachePath.empty()) {
                        primaryUsdNativeCachePath = materialPayload.value(
                            "cachePath",
                            genericRelativeOrValue(materialCache, workspace.root));
                    }
                    materialPayload["kind"] = "UsdMaterialBindingPayload";
                    materialPayload["materialNativeCookImplemented"] = true;
                    materialPayload["shaderNetworkConversionImplemented"] = shaderNetwork.shaderNetworkConverted;
                    materialPayload["textureReferenceExtractionImplemented"] = shaderNetwork.diagnostics.value("textureReferenceExtractionImplemented", false);
                    materialPayload["textureReferenceCount"] = shaderNetwork.diagnostics.value("textureReferenceCount", 0u);
                    materialPayload["unsupportedShaderNodeCount"] = materialUnsupportedShaderNodes.is_array() ? materialUnsupportedShaderNodes.size() : 0u;
                    materialPayload["unsupportedShaderNodes"] = materialUnsupportedShaderNodes;
                    materialPayload["shaderTextureMaterialBindingImplemented"] = materialTextureBindingCount > 0;
                    materialPayload["shaderTextureBindingCount"] = materialTextureBindingCount;
                    materialPayload["textureNativeCookImplemented"] = !materialTextureGuids.empty();
                    materialPayload["textureGuids"] = materialTextureGuids;
                    materialPayload["usdShaderTextureBindings"] = usdShaderTextureBindings;
                    materialPayload["sourceMaterialPath"] = materialPath;
                    materialPayload["usdShaderNetwork"] = shaderNetwork.diagnostics;
                    materialPayload["sourceNetworkDeduplicated"] = sourceNetworkDeduplicated;
                    materialPayload["materialDeduplicated"] = false;
                    materialPayload["sharedMaterialKey"] = materialKey;
                    materialPayload["uniqueMaterialIndex"] = uniqueMaterialIndex;
                    materialPayload["persistentCacheReused"] = materialPersistentCacheReused;
                    cookedPayloads.push_back(materialPayload);

                    (void)writeJson(materialPathJson, {
                        {"version", 1},
                        {"kind", "ImportedUsdMaterial"},
                        {"guid", materialGuid},
                        {"sourcePath", effectiveSourceString},
                        {"originalSourcePath", originalSourceString},
                        {"copiedSourcePath", copiedSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", materialPayload},
                        {"sourceMaterialPath", materialPath},
                        {"pbr", materialPbrMetadataJson(material)},
                        {"usdShaderNetwork", shaderNetwork.diagnostics},
                        {"usdMaterialBinding", {
                            {"materialBindingCookImplemented", true},
                            {"shaderNetworkConversionImplemented", shaderNetwork.shaderNetworkConverted},
                            {"textureReferenceExtractionImplemented", shaderNetwork.diagnostics.value("textureReferenceExtractionImplemented", false)},
                            {"textureReferenceCount", shaderNetwork.diagnostics.value("textureReferenceCount", 0u)},
                            {"unsupportedShaderNodeCount", materialUnsupportedShaderNodes.is_array() ? materialUnsupportedShaderNodes.size() : 0u},
                            {"unsupportedShaderNodes", materialUnsupportedShaderNodes},
                            {"shaderTextureMaterialBindingImplemented", materialTextureBindingCount > 0},
                            {"shaderTextureBindingCount", materialTextureBindingCount},
                            {"textureNativeCookImplemented", !materialTextureGuids.empty()},
                            {"usdShaderTextureBindings", usdShaderTextureBindings},
                            {"unsupportedShaderGraphPolicy", "non-PreviewSurface shader nodes are reported in unsupportedShaderNodes"},
                            {"sourceNetworkDeduplicated", sourceNetworkDeduplicated},
                            {"materialDeduplicated", false},
                            {"sharedMaterialKey", materialKey},
                            {"uniqueMaterialIndex", uniqueMaterialIndex},
                            {"persistentCacheReused", materialPersistentCacheReused},
                        }},
                    });

                    AssetRecord materialRecord;
                    materialRecord.guid = materialGuid;
                    materialRecord.type = AssetType::Material;
                    materialRecord.displayName = materialName;
                    materialRecord.sourcePath = effectiveSourceString;
                    materialRecord.importedPath = genericRelativeOrValue(materialPathJson, workspace.root);
                    materialRecord.cachePath = materialPayload.value("cachePath", genericRelativeOrValue(materialCache, workspace.root));
                    materialRecord.thumbnailPath = rootThumbnailPath;
                    materialRecord.sourceHash = sourceHash;
                    materialRecord.importSettingsHash = importSettingsHash;
                    materialRecord.lastModifiedTimestamp = timestampString();
                    materialRecord.importSettings = request.settings;
                    materialRecord.status = AssetImportStatus::Imported;
                    records.push_back(std::move(materialRecord));

                    usdMaterialAssets.push_back({
                        {"guid", materialGuid},
                        {"name", materialName},
                        {"path", genericRelativeOrValue(materialPathJson, workspace.root)},
                        {"cachePath", materialPayload.value("cachePath", std::string{})},
                        {"sourceMaterialPath", materialPath},
                        {"shaderNetworkConversionImplemented", shaderNetwork.shaderNetworkConverted},
                        {"textureReferenceExtractionImplemented", shaderNetwork.diagnostics.value("textureReferenceExtractionImplemented", false)},
                        {"textureReferenceCount", shaderNetwork.diagnostics.value("textureReferenceCount", 0u)},
                        {"unsupportedShaderNodeCount", materialUnsupportedShaderNodes.is_array() ? materialUnsupportedShaderNodes.size() : 0u},
                        {"unsupportedShaderNodes", materialUnsupportedShaderNodes},
                        {"shaderTextureMaterialBindingImplemented", materialTextureBindingCount > 0},
                        {"shaderTextureBindingCount", materialTextureBindingCount},
                        {"usdShaderTextureBindings", usdShaderTextureBindings},
                        {"usdShaderNetwork", shaderNetwork.diagnostics},
                        {"textureNativeCookImplemented", !materialTextureGuids.empty()},
                        {"sourceNetworkDeduplicated", sourceNetworkDeduplicated},
                        {"materialDeduplicated", false},
                        {"sharedMaterialKey", materialKey},
                        {"uniqueMaterialIndex", uniqueMaterialIndex},
                        {"persistentCacheReused", materialPersistentCacheReused},
                    });
                    usdCookedMaterialByContentKey.emplace(materialKey, UsdCookedMaterialReuseEntry{
                        .guid = materialGuid,
                        .name = materialName,
                        .path = genericRelativeOrValue(materialPathJson, workspace.root),
                        .cachePath = materialPayload.value("cachePath", std::string{}),
                        .sourceMaterialPath = materialPath,
                        .slot = materialSlot,
                        .requestIndex = materialRequestIndex,
                        .uniqueMaterialIndex = uniqueMaterialIndex,
                    });
                }
                usdMaterialDeduplication = {
                    {"enabled", true},
                    {"key", "native_material_content_texture_guid_slots"},
                    {"sourceMaterialCount", usdMaterialShaderNetworkCount},
                    {"sourceNetworkReusedCount", usdMaterialSourceNetworkReuseCount},
                    {"uniqueMaterialCount", usdUniqueMaterialCookCount},
                    {"reusedMaterialCount", usdReusedMaterialCookCount},
                    {"persistentCacheHitCount", usdMaterialPersistentCacheHitCount},
                    {"aliases", usdMaterialContentReuseAliases},
                };
            }
            if (usdRuntimeMeshes.supported && !usdRuntimeMeshes.meshes.empty()) {
                traceImport("USD import: cook decoded meshes");
                struct UsdCookedMeshReuseEntry {
                    AssetGuid guid;
                    std::string name;
                    std::string path;
                    std::string cachePath;
                    std::string sourcePrimPath;
                    size_t decodedMeshIndex = 0;
                    size_t uniqueGeometryIndex = 0;
                };
                std::unordered_map<std::string, UsdCookedMeshReuseEntry> usdCookedMeshByGeometryKey;
                nlohmann::json usdMeshGeometryReuseAliases = nlohmann::json::array();
                size_t usdUniqueMeshCookCount = 0;
                size_t usdReusedMeshCookCount = 0;
                size_t usdMeshPersistentCacheHitCount = 0;
                const nlohmann::json usdRuntimeMeshReports = usdRuntimeMeshes.diagnostics.value("meshes", nlohmann::json::array());
                setProgress(
                    0.76f,
                    "Cooking unique USD meshes 0 / " + std::to_string(usdRuntimeMeshes.meshes.size()));
                for (size_t i = 0; i < usdRuntimeMeshes.meshes.size(); ++i) {
                    const MeshAsset& usdMesh = usdRuntimeMeshes.meshes[i];
                    const float meshCookProgress = static_cast<float>(i + 1u) / static_cast<float>(usdRuntimeMeshes.meshes.size());
                    setProgress(
                        0.76f + 0.10f * meshCookProgress,
                        "Cooking unique USD meshes " + std::to_string(usdUniqueMeshCookCount) + " / " + std::to_string(usdRuntimeMeshes.meshes.size()) +
                            " (reused " + std::to_string(usdReusedMeshCookCount) + ")");
                    const nlohmann::json usdMeshCookDiagnostics = usdRuntimeMeshReports.is_array() && i < usdRuntimeMeshReports.size()
                        ? usdRuntimeMeshReports[i]
                        : nlohmann::json::object();
                    const std::string sourcePrimPath = usdMeshCookDiagnostics.value("primPath", std::string{});
                    const nlohmann::json sourcePrimTransform = usdMeshCookDiagnostics.value("transform", nlohmann::json::object());

                    MeshAsset usdMeshForCook = usdMesh;
                    const std::string materialBindingPath = i < usdRuntimeMeshes.materialBindingPaths.size()
                        ? usdRuntimeMeshes.materialBindingPaths[i]
                        : std::string{};
                    const auto materialSlotIt = usdMaterialSlotByPath.find(materialBindingPath);
                    const bool materialSlotBound = materialSlotIt != usdMaterialSlotByPath.end();
                    const std::vector<std::string> primitiveMaterialPaths = i < usdRuntimeMeshes.primitiveMaterialBindingPaths.size()
                        ? usdRuntimeMeshes.primitiveMaterialBindingPaths[i]
                        : std::vector<std::string>{};
                    nlohmann::json primitiveMaterialGuids = nlohmann::json::array();
                    bool anyPrimitiveMaterialBound = false;
                    for (size_t primitiveIndex = 0; primitiveIndex < usdMeshForCook.primitives.size(); ++primitiveIndex) {
                        std::string primitiveMaterialPath = primitiveIndex < primitiveMaterialPaths.size()
                            ? primitiveMaterialPaths[primitiveIndex]
                            : std::string{};
                        if (primitiveMaterialPath.empty()) {
                            primitiveMaterialPath = materialBindingPath;
                        }
                        const auto primitiveSlotIt = usdMaterialSlotByPath.find(primitiveMaterialPath);
                        const bool primitiveSlotBound = primitiveSlotIt != usdMaterialSlotByPath.end();
                        if (primitiveSlotBound) {
                            usdMeshForCook.primitives[primitiveIndex].material = MaterialAssetHandle{primitiveSlotIt->second};
                            anyPrimitiveMaterialBound = true;
                        } else if (materialSlotBound) {
                            usdMeshForCook.primitives[primitiveIndex].material = MaterialAssetHandle{materialSlotIt->second};
                            anyPrimitiveMaterialBound = true;
                        }
                        const uint32_t resolvedSlot = usdMeshForCook.primitives[primitiveIndex].material.index;
                        primitiveMaterialGuids.push_back({
                            {"primitiveIndex", primitiveIndex},
                            {"materialBindingPath", primitiveMaterialPath},
                            {"materialGuid", resolvedSlot < usdMaterialGuids.size() ? usdMaterialGuids[resolvedSlot] : std::string{}},
                            {"bound", resolvedSlot < usdMaterialGuids.size()},
                        });
                    }

                    const std::string geometryKey = meshGeometryDedupKey(usdMesh);
                    const nlohmann::json pointAnimation = usdMeshCookDiagnostics.value("pointAnimation", nlohmann::json::object());
                    const bool pointAnimated = pointAnimation.is_object() && pointAnimation.value("decoded", false);
                    const bool canReuseGeometry = !pointAnimated;
                    const auto reusableIt = canReuseGeometry ? usdCookedMeshByGeometryKey.find(geometryKey) : usdCookedMeshByGeometryKey.end();
                    if (reusableIt != usdCookedMeshByGeometryKey.end()) {
                        ++usdReusedMeshCookCount;
                        const UsdCookedMeshReuseEntry& shared = reusableIt->second;
                        nlohmann::json meshAlias = {
                            {"guid", shared.guid},
                            {"name", shared.name},
                            {"path", shared.path},
                            {"cachePath", shared.cachePath},
                            {"vertexCount", usdMeshForCook.vertices.size()},
                            {"indexCount", usdMeshForCook.indices.size()},
                            {"primitiveCount", usdMeshForCook.primitives.size()},
                            {"sourcePrimPath", sourcePrimPath},
                            {"sourcePrimTransform", sourcePrimTransform},
                            {"materialBindingPath", materialBindingPath},
                            {"materialGuid", materialSlotBound && materialSlotIt->second < usdMaterialGuids.size() ? usdMaterialGuids[materialSlotIt->second] : std::string{}},
                            {"primitiveMaterialBindings", primitiveMaterialGuids},
                            {"geometryDeduplicated", true},
                            {"sharedGeometryKey", geometryKey},
                            {"sharedGeometrySourcePrimPath", shared.sourcePrimPath},
                            {"sharedDecodedMeshIndex", shared.decodedMeshIndex},
                            {"uniqueGeometryIndex", shared.uniqueGeometryIndex},
                        };
                        usdMeshGeometryReuseAliases.push_back(meshAlias);
                        usdMeshAssets.push_back(std::move(meshAlias));
                        continue;
                    }

                    const AssetGuid meshGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "UsdMesh", i);
                    rootDependencies.push_back(meshGuid);
                    usdMeshGuids.push_back(meshGuid);
                    const size_t uniqueGeometryIndex = usdUniqueMeshCookCount++;
                    const std::string meshName = safeStem((usdMesh.name.empty() ? "UsdMesh" : usdMesh.name) + "_" + std::to_string(i));
                    const std::filesystem::path meshPath = importedDir / "Meshes" / (meshName + ".rtmesh.json");
                    const std::filesystem::path meshCache = cacheDir / "Meshes" / (meshName + ".rtmesh");
                    std::filesystem::create_directories(meshPath.parent_path(), ec);
                    std::filesystem::create_directories(meshCache.parent_path(), ec);

                    const std::optional<NativeAssetCookResult> cachedMeshCook = reusableNativeCookResult(
                        meshCache,
                        NativeAssetKind::Mesh,
                        meshGuid,
                        effectiveSourcePath);
                    const bool meshPersistentCacheReused = cachedMeshCook.has_value();
                    if (meshPersistentCacheReused) {
                        ++usdMeshPersistentCacheHitCount;
                    }
                    const NativeAssetCookResult meshCook = meshPersistentCacheReused
                        ? *cachedMeshCook
                        : nativeCooker.cookMesh(
                            nativeCookInput(meshGuid, meshCache, meshName),
                            usdMeshForCook,
                            usdMaterialGuids,
                            request.settings.buildBlasCache);
                    if (!recordNativeCookResult(meshCook, meshName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json meshPayload = nativeCookRuntimePayloadJson(
                        meshCook,
                        NativeAssetKind::Mesh,
                        meshGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    if (primaryUsdNativeCachePath.empty()) {
                        primaryUsdNativeCachePath = meshPayload.value(
                            "cachePath",
                            genericRelativeOrValue(meshCache, workspace.root));
                    }
                    meshPayload["kind"] = "UsdRuntimeMeshPayload";
                    meshPayload["runtimeGeometryCooked"] = true;
                    meshPayload["usdMeshTopologyDecoded"] = true;
                    meshPayload["materialSlotGuidBindingImplemented"] = materialSlotBound || anyPrimitiveMaterialBound;
                    meshPayload["materialSubsetGuidBindingImplemented"] = anyPrimitiveMaterialBound && primitiveMaterialPaths.size() > 1u;
                    meshPayload["materialGuid"] = materialSlotBound && materialSlotIt->second < usdMaterialGuids.size() ? usdMaterialGuids[materialSlotIt->second] : std::string{};
                    meshPayload["materialBindingPath"] = materialBindingPath;
                    meshPayload["primitiveMaterialBindings"] = primitiveMaterialGuids;
                    meshPayload["sourcePrimPath"] = sourcePrimPath;
                    meshPayload["sourcePrimTransform"] = sourcePrimTransform;
                    meshPayload["geometryDeduplicated"] = false;
                    meshPayload["sharedGeometryKey"] = geometryKey;
                    meshPayload["uniqueGeometryIndex"] = uniqueGeometryIndex;
                    meshPayload["geometryDeduplicationEligible"] = canReuseGeometry;
                    meshPayload["geometryDeduplicationSkipReason"] = canReuseGeometry ? std::string{} : std::string{"point_animated_mesh"};
                    meshPayload["persistentCacheReused"] = meshPersistentCacheReused;
                    meshPayload["localBvhCacheRequested"] = request.settings.buildBlasCache;
                    cookedPayloads.push_back(meshPayload);

                    (void)writeJson(meshPath, {
                        {"version", 1},
                        {"kind", "ImportedUsdMesh"},
                        {"guid", meshGuid},
                        {"sourcePath", effectiveSourceString},
                        {"originalSourcePath", originalSourceString},
                        {"copiedSourcePath", copiedSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", meshPayload},
                        {"sourcePrimPath", meshPayload.value("sourcePrimPath", std::string{})},
                        {"sourcePrimTransform", meshPayload.value("sourcePrimTransform", nlohmann::json::object())},
                        {"vertexCount", usdMeshForCook.vertices.size()},
                        {"indexCount", usdMeshForCook.indices.size()},
                        {"primitiveCount", usdMeshForCook.primitives.size()},
                        {"geometryDeduplication", {
                            {"geometryDeduplicated", false},
                            {"sharedGeometryKey", geometryKey},
                            {"uniqueGeometryIndex", uniqueGeometryIndex},
                            {"eligible", canReuseGeometry},
                            {"skipReason", canReuseGeometry ? std::string{} : std::string{"point_animated_mesh"}},
                            {"persistentCacheReused", meshPersistentCacheReused},
                            {"localBvhCacheRequested", request.settings.buildBlasCache},
                        }},
                        {"usdRuntimeMeshCook", usdMeshCookDiagnostics},
                        {"materialBinding", {
                            {"materialNativeCookImplemented", materialSlotBound || anyPrimitiveMaterialBound},
                            {"materialSlotGuidBindingImplemented", materialSlotBound || anyPrimitiveMaterialBound},
                            {"materialSubsetGuidBindingImplemented", anyPrimitiveMaterialBound && primitiveMaterialPaths.size() > 1u},
                            {"materialBindingPath", materialBindingPath},
                            {"materialGuid", materialSlotBound && materialSlotIt->second < usdMaterialGuids.size() ? usdMaterialGuids[materialSlotIt->second] : std::string{}},
                            {"primitiveMaterialBindings", primitiveMaterialGuids},
                            {"shaderNetworkConversionImplemented", materialSlotBound || anyPrimitiveMaterialBound},
                            {"shaderTextureBindingImplemented", materialSlotBound || anyPrimitiveMaterialBound},
                        }},
                    });

                    AssetRecord meshRecord;
                    meshRecord.guid = meshGuid;
                    meshRecord.type = AssetType::Mesh;
                    meshRecord.displayName = meshName;
                    meshRecord.sourcePath = effectiveSourceString;
                    meshRecord.importedPath = genericRelativeOrValue(meshPath, workspace.root);
                    meshRecord.cachePath = meshPayload.value("cachePath", genericRelativeOrValue(meshCache, workspace.root));
                    meshRecord.thumbnailPath = rootThumbnailPath;
                    meshRecord.sourceHash = sourceHash;
                    meshRecord.importSettingsHash = importSettingsHash;
                    meshRecord.lastModifiedTimestamp = timestampString();
                    meshRecord.importSettings = request.settings;
                    meshRecord.status = AssetImportStatus::Imported;
                    records.push_back(std::move(meshRecord));

                    nlohmann::json meshAssetSummary = {
                        {"guid", meshGuid},
                        {"name", meshName},
                        {"path", genericRelativeOrValue(meshPath, workspace.root)},
                        {"cachePath", meshPayload.value("cachePath", std::string{})},
                        {"vertexCount", usdMeshForCook.vertices.size()},
                        {"indexCount", usdMeshForCook.indices.size()},
                        {"primitiveCount", usdMeshForCook.primitives.size()},
                        {"sourcePrimPath", meshPayload.value("sourcePrimPath", std::string{})},
                        {"sourcePrimTransform", meshPayload.value("sourcePrimTransform", nlohmann::json::object())},
                        {"materialBindingPath", materialBindingPath},
                        {"materialGuid", materialSlotBound && materialSlotIt->second < usdMaterialGuids.size() ? usdMaterialGuids[materialSlotIt->second] : std::string{}},
                        {"primitiveMaterialBindings", primitiveMaterialGuids},
                        {"geometryDeduplicated", false},
                        {"sharedGeometryKey", geometryKey},
                        {"uniqueGeometryIndex", uniqueGeometryIndex},
                        {"persistentCacheReused", meshPersistentCacheReused},
                        {"localBvhCacheRequested", request.settings.buildBlasCache},
                    };
                    usdMeshAssets.push_back(meshAssetSummary);
                    if (canReuseGeometry) {
                        usdCookedMeshByGeometryKey.emplace(geometryKey, UsdCookedMeshReuseEntry{
                            .guid = meshGuid,
                            .name = meshName,
                            .path = meshAssetSummary.value("path", std::string{}),
                            .cachePath = meshAssetSummary.value("cachePath", std::string{}),
                            .sourcePrimPath = sourcePrimPath,
                            .decodedMeshIndex = i,
                            .uniqueGeometryIndex = uniqueGeometryIndex,
                        });
                    }
                }
                usdRuntimeMeshes.diagnostics["geometryDeduplication"] = {
                    {"enabled", true},
                    {"key", "vertices_indices_primitive_ranges"},
                    {"decodedMeshCount", usdRuntimeMeshes.meshes.size()},
                    {"uniqueGeometryCount", usdUniqueMeshCookCount},
                    {"reusedGeometryCount", usdReusedMeshCookCount},
                    {"persistentCacheHitCount", usdMeshPersistentCacheHitCount},
                    {"localBvhCacheRequested", request.settings.buildBlasCache},
                    {"localBvhBuildPolicy", request.settings.buildBlasCache ? "build_once_per_unique_geometry_or_reuse_validated_cache" : "runtime_lazy_build"},
                    {"pointAnimatedMeshDeduplicationPolicy", "disabled_to_preserve_per_prim_vertex_animation_tracks"},
                    {"aliases", usdMeshGeometryReuseAliases},
                };
            }
            setProgress(0.86f, "Writing USD animation metadata");
            nlohmann::json usdAnimations = usdData.animations;
            uint32_t decodedMeshPointChannelCount = 0;
            uint32_t decodedMeshPointKeyframeCount = 0;
            nlohmann::json meshPointChannels = nlohmann::json::array();
            std::unordered_map<std::string, int> usdPrimIndexByPath;
            for (const nlohmann::json& prim : usdData.prims) {
                if (!prim.is_object()) {
                    continue;
                }
                const std::string path = prim.value("path", std::string{});
                if (!path.empty()) {
                    usdPrimIndexByPath[path] = prim.value("index", -1);
                }
            }
            const nlohmann::json meshReports = usdRuntimeMeshes.diagnostics.value("meshes", nlohmann::json::array());
            if (meshReports.is_array()) {
                for (const nlohmann::json& meshReport : meshReports) {
                    if (!meshReport.is_object()) {
                        continue;
                    }
                    const nlohmann::json pointAnimation = meshReport.value("pointAnimation", nlohmann::json::object());
                    if (!pointAnimation.value("decoded", false)) {
                        continue;
                    }
                    const std::string primPath = meshReport.value("primPath", std::string{});
                    const auto primIt = usdPrimIndexByPath.find(primPath);
                    if (primIt == usdPrimIndexByPath.end() || primIt->second < 0) {
                        continue;
                    }
                    nlohmann::json times = nlohmann::json::array();
                    const nlohmann::json timeCodes = pointAnimation.value("timeCodes", nlohmann::json::array());
                    if (!timeCodes.is_array()) {
                        continue;
                    }
                    for (const nlohmann::json& value : timeCodes) {
                        if (value.is_number()) {
                            const double timeCode = value.get<double>();
                            times.push_back(usdData.timeCodesPerSecond > 0.0 ? timeCode / usdData.timeCodesPerSecond : timeCode);
                        }
                    }
                    const nlohmann::json values = pointAnimation.value("values", nlohmann::json::array());
                    if (times.size() <= 1u || !values.is_array() || values.size() != times.size()) {
                        continue;
                    }
                    const std::string targetName = meshReport.value("name", std::string("UsdMesh"));
                    appendUsdDecodedParameterChannel(
                        meshPointChannels,
                        decodedMeshPointChannelCount,
                        decodedMeshPointKeyframeCount,
                        targetName,
                        primIt->second,
                        "meshVertexPositions",
                        std::move(times),
                        values);
                }
            }
            if (!meshPointChannels.empty()) {
                if (usdAnimations.empty()) {
                    usdAnimations.push_back({
                        {"schema", "UsdAnimationClipV1"},
                        {"sourceFormat", "USD"},
                        {"name", safeStem(name + "_UsdAnimation")},
                        {"clip", {
                            {"startTime", 0.0},
                            {"endTime", 0.0},
                            {"duration", 0.0},
                            {"timeCodesPerSecond", usdData.timeCodesPerSecond},
                            {"sourceStartTimeCode", usdData.startTimeCode},
                            {"sourceEndTimeCode", usdData.endTimeCode},
                        }},
                        {"channelCount", 0u},
                        {"decodedChannelCount", 0u},
                        {"decodedKeyframeCount", 0u},
                        {"channels", nlohmann::json::array()},
                        {"runtimeSupport", "decoded_usd_mesh_point_keyframes_runtime_playback_supported"},
                    });
                }
                nlohmann::json& animation = usdAnimations.front();
                nlohmann::json channels = animation.value("channels", nlohmann::json::array());
                for (nlohmann::json& channel : meshPointChannels) {
                    channel["index"] = channels.size();
                    channels.push_back(std::move(channel));
                }
                double clipStart = std::numeric_limits<double>::max();
                double clipEnd = 0.0;
                for (const nlohmann::json& channel : channels) {
                    const nlohmann::json decodedTrack = channel.value("decodedTrack", nlohmann::json::object());
                    const nlohmann::json times = decodedTrack.value("times", nlohmann::json::array());
                    if (!times.empty()) {
                        clipStart = std::min(clipStart, times.front().get<double>());
                        clipEnd = std::max(clipEnd, times.back().get<double>());
                    }
                }
                if (clipStart == std::numeric_limits<double>::max()) {
                    clipStart = 0.0;
                }
                animation["channels"] = channels;
                animation["channelCount"] = channels.size();
                animation["decodedChannelCount"] = animation.value("decodedChannelCount", 0u) + decodedMeshPointChannelCount;
                animation["decodedKeyframeCount"] = animation.value("decodedKeyframeCount", 0u) + decodedMeshPointKeyframeCount;
                animation["decodedMeshPointChannelCount"] = decodedMeshPointChannelCount;
                animation["runtimeSupport"] = "decoded_usd_transform_mesh_point_camera_light_keyframes_runtime_playback_supported";
                animation["clip"]["startTime"] = clipStart;
                animation["clip"]["endTime"] = clipEnd;
                animation["clip"]["duration"] = std::max(0.0, clipEnd - clipStart);
            }
            usdData.decodedMeshPointAnimationChannelCount = decodedMeshPointChannelCount;
            usdData.decodedMeshPointAnimationKeyframeCount = decodedMeshPointKeyframeCount;
            if (usdData.diagnostics.contains("animation") && usdData.diagnostics["animation"].is_object()) {
                usdData.diagnostics["animation"]["decodedMeshPointAnimationChannelCount"] = decodedMeshPointChannelCount;
                usdData.diagnostics["animation"]["decodedMeshPointAnimationKeyframeCount"] = decodedMeshPointKeyframeCount;
                usdData.diagnostics["animation"]["runtimeMeshPointPlaybackImplemented"] = decodedMeshPointChannelCount > 0;
                usdData.diagnostics["animation"]["runtimePlaybackImplemented"] =
                    (usdData.decodedTransformAnimationChannelCount +
                     usdData.decodedCameraLightAnimationChannelCount +
                     decodedMeshPointChannelCount) > 0;
            }
            usdData.diagnostics["animations"] = usdAnimations;

            for (size_t i = 0; i < usdAnimations.size(); ++i) {
                const nlohmann::json& animation = usdAnimations[i];
                const AssetGuid animationGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "UsdAnimation", i);
                rootDependencies.push_back(animationGuid);
                const std::string authoredAnimationName = animation.value("name", std::string{});
                const std::string animationName = safeStem(authoredAnimationName.empty() ? ("UsdAnimation_" + std::to_string(i)) : authoredAnimationName);
                const std::filesystem::path animationPath = importedDir / "Animations" / (animationName + ".rtanim.json");
                const std::filesystem::path nativeAnimationPath = cacheDir / "Animations" / (animationName + ".rtanim");
                std::filesystem::create_directories(animationPath.parent_path(), ec);
                std::filesystem::create_directories(nativeAnimationPath.parent_path(), ec);

                nlohmann::json nativeAnimationMetadata = animation;
                nativeAnimationMetadata["assetGuid"] = animationGuid;
                nativeAnimationMetadata["assetIndex"] = i;
                nativeAnimationMetadata["sourceFormat"] = "USD";
                const NativeAssetCookResult animationCook = nativeCooker.cookMetadataPayload(
                    nativeCookInput(animationGuid, nativeAnimationPath, animationName),
                    NativeAssetKind::Animation,
                    nativeAnimationMetadata);
                if (!recordNativeCookResult(animationCook, animationName)) {
                    result.workerTotalMs = elapsedMilliseconds(workerStart);
                    return result;
                }
                nlohmann::json animationPayload = nativeCookRuntimePayloadJson(
                    animationCook,
                    NativeAssetKind::Animation,
                    animationGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                animationPayload["assetIndex"] = i;
                animationPayload["kind"] = "UsdTransformAnimationMetadataPayload";
                animationPayload["runtimeSupport"] = animation.value("runtimeSupport", std::string("decoded_usd_transform_keyframes_runtime_playback_supported"));
                cookedPayloads.push_back(animationPayload);
                (void)writeJson(animationPath, {
                    {"version", 1},
                    {"kind", "ImportedUsdAnimation"},
                    {"guid", animationGuid},
                    {"sourcePath", effectiveSourceString},
                    {"originalSourcePath", originalSourceString},
                    {"copiedSourcePath", copiedSourceString},
                    {"sourceHash", sourceHash},
                    {"importSettingsHash", importSettingsHash},
                    {"runtimePayload", animationPayload},
                    {"animation", animation},
                    {"runtimeSupport", animation.value("runtimeSupport", std::string("decoded_usd_transform_keyframes_runtime_playback_supported"))},
                });
                generatedUsdAnimationAssets.push_back({
                    {"guid", animationGuid},
                    {"name", animationName},
                    {"path", genericRelativeOrValue(animationPath, workspace.root)},
                    {"channelCount", animation.value("channelCount", 0u)},
                    {"decodedChannelCount", animation.value("decodedChannelCount", 0u)},
                    {"decodedKeyframeCount", animation.value("decodedKeyframeCount", 0u)},
                });

                AssetRecord animationRecord;
                animationRecord.guid = animationGuid;
                animationRecord.type = AssetType::Animation;
                animationRecord.displayName = animationName;
                animationRecord.sourcePath = effectiveSourceString;
                animationRecord.importedPath = genericRelativeOrValue(animationPath, workspace.root);
                animationRecord.cachePath = animationPayload.value("cachePath", std::string{});
                animationRecord.thumbnailPath = rootThumbnailPath;
                animationRecord.sourceHash = sourceHash;
                animationRecord.importSettingsHash = importSettingsHash;
                animationRecord.lastModifiedTimestamp = timestampString();
                animationRecord.importSettings = request.settings;
                animationRecord.status = AssetImportStatus::Imported;
                records.push_back(std::move(animationRecord));
            }
            if (!generatedUsdAnimationAssets.empty()) {
                const nlohmann::json& firstAnimation = generatedUsdAnimationAssets.front();
                const std::string controllerName = safeStem(name + "_UsdController");
                const AssetGuid controllerGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "UsdAnimationController", 0);
                const std::filesystem::path controllerPath = importedDir / "AnimationControllers" / (controllerName + ".rtanimcontroller.json");
                const std::filesystem::path nativeControllerPath = cacheDir / "AnimationControllers" / (controllerName + ".rtanimcontroller");
                std::filesystem::create_directories(controllerPath.parent_path(), ec);
                std::filesystem::create_directories(nativeControllerPath.parent_path(), ec);
                const std::string clipGuid = firstAnimation.value("guid", std::string{});
                const std::string clipName = firstAnimation.value("name", std::string("Default"));
                nlohmann::json controllerJson = {
                    {"version", 1},
                    {"kind", "ImportedUsdAnimationController"},
                    {"guid", controllerGuid},
                    {"controller", {
                        {"name", controllerName},
                        {"states", nlohmann::json::array({{
                            {"name", clipName.empty() ? std::string("Default") : clipName},
                            {"clipGuid", clipGuid},
                            {"speed", 1.0f},
                            {"loop", true},
                            {"default", true},
                        }})},
                    }},
                };
                std::vector<std::string> controllerWarnings;
                const AnimationController controller = AnimationController::fromJson(controllerJson, &controllerWarnings);
                if (controller.valid()) {
                    const NativeAssetCookResult controllerCook = nativeCooker.cookAnimationController(
                        nativeCookInput(controllerGuid, nativeControllerPath, controllerName),
                        controller);
                    if (!recordNativeCookResult(controllerCook, controllerName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json controllerPayload = nativeCookRuntimePayloadJson(
                        controllerCook,
                        NativeAssetKind::AnimationController,
                        controllerGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    controllerPayload["kind"] = "UsdAnimationControllerPayload";
                    controllerPayload["clipGuid"] = clipGuid;
                    controllerPayload["animationControllerBindingImplemented"] = true;
                    cookedPayloads.push_back(controllerPayload);
                    controllerJson["sourcePath"] = effectiveSourceString;
                    controllerJson["originalSourcePath"] = originalSourceString;
                    controllerJson["copiedSourcePath"] = copiedSourceString;
                    controllerJson["sourceHash"] = sourceHash;
                    controllerJson["importSettingsHash"] = importSettingsHash;
                    controllerJson["runtimePayload"] = controllerPayload;
                    (void)writeJson(controllerPath, controllerJson);

                    generatedUsdAnimationControllerAssets.push_back({
                        {"guid", controllerGuid},
                        {"name", controllerName},
                        {"path", genericRelativeOrValue(controllerPath, workspace.root)},
                        {"clipGuid", clipGuid},
                        {"stateCount", 1},
                    });

                    AssetRecord controllerRecord;
                    controllerRecord.guid = controllerGuid;
                    controllerRecord.type = AssetType::AnimationController;
                    controllerRecord.displayName = controllerName;
                    controllerRecord.sourcePath = effectiveSourceString;
                    controllerRecord.importedPath = genericRelativeOrValue(controllerPath, workspace.root);
                    controllerRecord.cachePath = controllerPayload.value("cachePath", std::string{});
                    controllerRecord.thumbnailPath = rootThumbnailPath;
                    controllerRecord.sourceHash = sourceHash;
                    controllerRecord.importSettingsHash = importSettingsHash;
                    controllerRecord.lastModifiedTimestamp = timestampString();
                    controllerRecord.importSettings = request.settings;
                    controllerRecord.status = AssetImportStatus::Imported;
                    if (!clipGuid.empty()) {
                        controllerRecord.dependencies.push_back(AssetDependency{clipGuid, "animationClip"});
                    }
                    records.push_back(std::move(controllerRecord));
                } else {
                    result.warnings.push_back("USD animation controller bridge was skipped because generated controller JSON was invalid.");
                    for (const std::string& warning : controllerWarnings) {
                        result.warnings.push_back(warning);
                    }
                }
            }
            usdMetadata["runtimeMeshCook"] = usdRuntimeMeshes.diagnostics;
            usdMetadata["meshNativeCookImplemented"] = !usdMeshGuids.empty();
            usdMetadata["meshNativeCookAssetCount"] = usdMeshGuids.size();
            usdMetadata["materialNativeCookImplemented"] = !usdMaterialGuids.empty();
            usdMetadata["materialNativeCookAssetCount"] = usdMaterialGuids.size();
            usdMetadata["materialBindingCookImplemented"] = !usdMaterialGuids.empty();
            usdMetadata["materialDeduplication"] = usdMaterialDeduplication;
            usdMetadata["shaderNetworkConversionImplemented"] = usdShaderNetworkConverted;
            usdMetadata["textureReferenceExtractionImplemented"] = usdTextureReferenceExtractionImplemented;
            usdMetadata["textureReferenceCount"] = usdTextureReferenceCount;
            usdMetadata["unsupportedShaderNodeCount"] = usdUnsupportedShaderNodeCount;
            usdMetadata["unsupportedShaderNodes"] = usdUnsupportedShaderNodes;
            usdMetadata["shaderTextureMaterialBindingImplemented"] = usdShaderTextureMaterialBindingImplemented;
            usdMetadata["shaderTextureBindingCount"] = usdShaderTextureBindingCount;
            usdMetadata["usdExternalTextureNativeCookImplemented"] = usdExternalTextureNativeCookImplemented;
            usdMetadata["usdExternalTextureCookCount"] = usdExternalTextureGuids.size();
            usdMetadata["usdExternalTextureSingleFlightReuseCount"] = usdExternalTextureSingleFlightReuseCount;
            usdMetadata["usdExternalTexturePersistentCacheHitCount"] = usdExternalTexturePersistentCacheHitCount;
            usdMetadata["usdExternalTextureAssets"] = usdExternalTextureAssets;
            usdMetadata["textureNativeCookImplemented"] = usdShaderTextureMaterialBindingImplemented || usdzTextureNativeCookImplemented || usdExternalTextureNativeCookImplemented;
            usdMetadata["runtimeSceneEntities"] = usdSceneEntities.diagnostics;
            usdMetadata["cameraRuntimeConversionImplemented"] = usdSceneEntities.cameraCount > 0;
            usdMetadata["lightRuntimeConversionImplemented"] = usdSceneEntities.lightCount > 0;
            usdMetadata["runtimeCameraCount"] = usdSceneEntities.cameraCount;
            usdMetadata["runtimeLightCount"] = usdSceneEntities.lightCount;
            usdMetadata["runtimePackagePlacementImplemented"] = true;
            usdMetadata["runtimeReloadParityImplemented"] = true;
            usdMetadata["viewportPlacementImplemented"] = true;
            usdMetadata["animationAssets"] = generatedUsdAnimationAssets;
            usdMetadata["animationControllerAssets"] = generatedUsdAnimationControllerAssets;
            usdMetadata["runtimeTransformAnimationPlaybackImplemented"] = !generatedUsdAnimationAssets.empty() && !generatedUsdAnimationControllerAssets.empty();
            usdMetadata["runtimeMeshPointAnimationPlaybackImplemented"] = decodedMeshPointChannelCount > 0 && !generatedUsdAnimationControllerAssets.empty();
            usdMetadata["runtimeCameraLightParameterPlaybackImplemented"] = usdData.decodedCameraLightAnimationChannelCount > 0 && !generatedUsdAnimationControllerAssets.empty();
            usdMetadata["decodedMeshPointAnimationChannelCount"] = decodedMeshPointChannelCount;
            usdMetadata["decodedCameraLightAnimationChannelCount"] = usdData.decodedCameraLightAnimationChannelCount;
            usdMetadata["animationControllerBindingImplemented"] = !generatedUsdAnimationControllerAssets.empty();

            std::unordered_map<std::string, nlohmann::json> usdMeshAssetByPrimPath;
            for (const nlohmann::json& meshAsset : usdMeshAssets) {
                if (!meshAsset.is_object()) {
                    continue;
                }
                const std::string primPath = meshAsset.value("sourcePrimPath", std::string{});
                if (!primPath.empty()) {
                    usdMeshAssetByPrimPath[primPath] = meshAsset;
                }
            }
            std::unordered_map<std::string, nlohmann::json> usdCameraByPrimPath;
            if (usdSceneEntities.cameras.is_array()) {
                for (const nlohmann::json& camera : usdSceneEntities.cameras) {
                    if (!camera.is_object()) {
                        continue;
                    }
                    const std::string primPath = camera.value("primPath", std::string{});
                    if (!primPath.empty()) {
                        usdCameraByPrimPath[primPath] = camera;
                    }
                }
            }
            std::unordered_map<std::string, nlohmann::json> usdLightByPrimPath;
            if (usdSceneEntities.lights.is_array()) {
                for (const nlohmann::json& light : usdSceneEntities.lights) {
                    if (!light.is_object()) {
                        continue;
                    }
                    const std::string primPath = light.value("primPath", std::string{});
                    if (!primPath.empty()) {
                        usdLightByPrimPath[primPath] = light;
                    }
                }
            }
            nlohmann::json usdPrefabRootNodes = nlohmann::json::array();
            std::vector<nlohmann::json> usdPrefabNodeList;
            usdPrefabNodeList.reserve(usdData.prims.size());
            std::unordered_map<std::string, size_t> usdPrefabIndexByPrimPath;
            for (const nlohmann::json& prim : usdData.prims) {
                if (!prim.is_object()) {
                    continue;
                }
                const std::string primPath = prim.value("path", std::string{});
                const std::string typeName = prim.value("typeName", std::string{});
                if (primPath.empty() || typeName == "Material" || typeName == "Shader") {
                    continue;
                }
                const std::string parentPath = prim.value("parentPath", std::string{});
                int parentIndex = -1;
                if (const auto parentIt = usdPrefabIndexByPrimPath.find(parentPath); parentIt != usdPrefabIndexByPrimPath.end()) {
                    parentIndex = static_cast<int>(parentIt->second);
                }
                const size_t prefabIndex = usdPrefabNodeList.size();
                nlohmann::json node = {
                    {"index", prefabIndex},
                    {"sourceNodeIndex", prim.value("index", static_cast<int>(prefabIndex))},
                    {"sourcePrimPath", primPath},
                    {"name", prim.value("name", std::string("UsdPrim_" + std::to_string(prefabIndex)))},
                    {"parent", parentIndex},
                    {"children", nlohmann::json::array()},
                    {"typeName", typeName},
                    {"visible", prim.value("visible", true)},
                    {"purpose", prim.value("purpose", std::string("default"))},
                    {"meshGuid", std::string{}},
                    {"materialGuids", nlohmann::json::array()},
                    {"hasCamera", false},
                    {"hasLight", false},
                };
                const nlohmann::json primTransform = prim.value("transform", nlohmann::json::object());
                if (primTransform.is_object()) {
                    node["transform"] = primTransform.value("placementTransform", nlohmann::json::object());
                }
                if (const auto meshIt = usdMeshAssetByPrimPath.find(primPath); meshIt != usdMeshAssetByPrimPath.end()) {
                    const nlohmann::json& meshAsset = meshIt->second;
                    node["meshGuid"] = meshAsset.value("guid", std::string{});
                    const std::string materialGuid = meshAsset.value("materialGuid", std::string{});
                    const nlohmann::json primitiveBindings = meshAsset.value("primitiveMaterialBindings", nlohmann::json::array());
                    const size_t primitiveCount = meshAsset.value("primitiveCount", static_cast<size_t>(0));
                    if (primitiveBindings.is_array()) {
                        std::vector<std::string> materialGuidsForPrimitives(primitiveCount, materialGuid);
                        for (const nlohmann::json& binding : primitiveBindings) {
                            if (!binding.is_object()) {
                                continue;
                            }
                            const size_t primitiveIndex = binding.value("primitiveIndex", static_cast<size_t>(materialGuidsForPrimitives.size()));
                            const std::string primitiveMaterialGuid = binding.value("materialGuid", std::string{});
                            if (primitiveIndex >= materialGuidsForPrimitives.size()) {
                                materialGuidsForPrimitives.resize(primitiveIndex + 1u, materialGuid);
                            }
                            if (!primitiveMaterialGuid.empty()) {
                                materialGuidsForPrimitives[primitiveIndex] = primitiveMaterialGuid;
                            }
                        }
                        for (const std::string& primitiveMaterialGuid : materialGuidsForPrimitives) {
                            node["materialGuids"].push_back(primitiveMaterialGuid);
                        }
                    } else if (!materialGuid.empty()) {
                        const size_t materialSlotCount = std::max<size_t>(primitiveCount, 1u);
                        for (size_t slotIndex = 0; slotIndex < materialSlotCount; ++slotIndex) {
                            node["materialGuids"].push_back(materialGuid);
                        }
                    }
                }
                if (const auto cameraIt = usdCameraByPrimPath.find(primPath); cameraIt != usdCameraByPrimPath.end()) {
                    const nlohmann::json& camera = cameraIt->second;
                    node["hasCamera"] = true;
                    node["cameraProjection"] = camera.value("cameraProjection", 0u);
                    node["cameraYfov"] = camera.value("cameraYfov", 60.0f * 0.017453292519943295f);
                    node["cameraAspectRatio"] = camera.value("cameraAspectRatio", 0.0f);
                    node["cameraOrthoXmag"] = camera.value("cameraOrthoXmag", 1.0f);
                    node["cameraOrthoYmag"] = camera.value("cameraOrthoYmag", 1.0f);
                    node["cameraNear"] = camera.value("cameraNear", 0.01f);
                    node["cameraFar"] = camera.value("cameraFar", 1000.0f);
                }
                if (const auto lightIt = usdLightByPrimPath.find(primPath); lightIt != usdLightByPrimPath.end()) {
                    const nlohmann::json& light = lightIt->second;
                    node["hasLight"] = true;
                    node["lightType"] = light.value("lightType", 1u);
                    node["lightColor"] = light.value("color", nlohmann::json::array({1.0f, 1.0f, 1.0f}));
                    node["lightIntensity"] = light.value("intensity", 1.0f);
                    node["lightSizeOrRadius"] = light.value("sizeOrRadius", 1.0f);
                    node["lightInnerConeRadians"] = light.value("innerConeRadians", 0.35f);
                    node["lightOuterConeRadians"] = light.value("outerConeRadians", 0.70f);
                    node["lightEnabled"] = light.value("enabled", true);
                }
                usdPrefabIndexByPrimPath.emplace(primPath, prefabIndex);
                if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < usdPrefabNodeList.size()) {
                    usdPrefabNodeList[static_cast<size_t>(parentIndex)]["children"].push_back(prefabIndex);
                } else {
                    usdPrefabRootNodes.push_back(prefabIndex);
                }
                usdPrefabNodeList.push_back(std::move(node));
            }
            nlohmann::json usdPrefabNodes = nlohmann::json::array();
            for (nlohmann::json& node : usdPrefabNodeList) {
                usdPrefabNodes.push_back(std::move(node));
            }
            usdMetadata["prefabRootNodes"] = usdPrefabRootNodes;
            usdMetadata["prefabNodeCount"] = usdPrefabNodes.size();

            runtimePayload = {
                {"kind", "UsdStageMetadataPayload"},
                {"cachePath", primaryUsdNativeCachePath},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"available", true},
                {"validForSource", true},
                {"prefabAssetImplemented", true},
                {"rootNodes", usdPrefabRootNodes},
                {"nodes", usdPrefabNodes},
                {"sceneGraphMetadataImportImplemented", true},
                {"meshNativeCookImplemented", !usdMeshGuids.empty()},
                {"meshNativeCookAssetCount", usdMeshGuids.size()},
                {"meshAssets", usdMeshAssets},
                {"materialNativeCookImplemented", !usdMaterialGuids.empty()},
                {"materialNativeCookAssetCount", usdMaterialGuids.size()},
                {"materialBindingCookImplemented", !usdMaterialGuids.empty()},
                {"materialDeduplication", usdMaterialDeduplication},
                {"materialAssets", usdMaterialAssets},
                {"shaderNetworkConversionImplemented", usdShaderNetworkConverted},
                {"textureReferenceExtractionImplemented", usdTextureReferenceExtractionImplemented},
                {"textureReferenceCount", usdTextureReferenceCount},
                {"unsupportedShaderNodeCount", usdUnsupportedShaderNodeCount},
                {"unsupportedShaderNodes", usdUnsupportedShaderNodes},
                {"shaderTextureMaterialBindingImplemented", usdShaderTextureMaterialBindingImplemented},
                {"shaderTextureBindingCount", usdShaderTextureBindingCount},
                {"usdExternalTextureNativeCookImplemented", usdExternalTextureNativeCookImplemented},
                {"usdExternalTextureCookCount", usdExternalTextureGuids.size()},
                {"usdExternalTextureSingleFlightReuseCount", usdExternalTextureSingleFlightReuseCount},
                {"usdExternalTexturePersistentCacheHitCount", usdExternalTexturePersistentCacheHitCount},
                {"usdExternalTextureAssets", usdExternalTextureAssets},
                {"textureNativeCookImplemented", usdShaderTextureMaterialBindingImplemented || usdzTextureNativeCookImplemented || usdExternalTextureNativeCookImplemented},
                {"usdzTextureProvenanceInspectionImplemented", usdzPackageTextures.inspected},
                {"usdzPackagedTextureEntryCount", usdzPackageTextures.textureEntryCount},
                {"usdzTextureExtractionImplemented", usdzTextureExtractionImplemented},
                {"usdzTextureNativeCookImplemented", usdzTextureNativeCookImplemented},
                {"usdzExtractedTextureCount", usdzExtractedTextureCount},
                {"usdzNativeTextureCookCount", usdzNativeTextureCookCount},
                {"usdzTexturePersistentCacheHitCount", usdzTexturePersistentCacheHitCount},
                {"usdzTextureAssets", usdzTextureAssets},
                {"usdzPackageTextures", usdzPackageTextures.diagnostics},
                {"cameraRuntimeConversionImplemented", usdSceneEntities.cameraCount > 0},
                {"lightRuntimeConversionImplemented", usdSceneEntities.lightCount > 0},
                {"runtimeCameras", usdSceneEntities.cameras},
                {"runtimeLights", usdSceneEntities.lights},
                {"animationAssets", generatedUsdAnimationAssets},
                {"animationControllerAssets", generatedUsdAnimationControllerAssets},
                {"runtimeTransformAnimationPlaybackImplemented", !generatedUsdAnimationAssets.empty() && !generatedUsdAnimationControllerAssets.empty()},
                {"animationControllerBindingImplemented", !generatedUsdAnimationControllerAssets.empty()},
                {"runtimeMeshPointAnimationPlaybackImplemented", decodedMeshPointChannelCount > 0 && !generatedUsdAnimationControllerAssets.empty()},
                {"runtimeCameraLightParameterPlaybackImplemented", usdData.decodedCameraLightAnimationChannelCount > 0 && !generatedUsdAnimationControllerAssets.empty()},
                {"decodedMeshPointAnimationChannelCount", decodedMeshPointChannelCount},
                {"decodedCameraLightAnimationChannelCount", usdData.decodedCameraLightAnimationChannelCount},
                {"viewportPlacementImplemented", true},
                {"runtimePackagePlacementImplemented", true},
                {"runtimeReloadParityImplemented", true},
                {"runtimePlacementPolicy", "USD import emits stage hierarchy, mesh/material payloads, cameras, lights, and transform animation clips; Import and Place builds tagged USD prim entities, attaches native mesh payloads and animation controllers, so reimport can refresh placed entities by source prim path and GUID."},
                {"usdStageImport", usdData.diagnostics},
                {"usdRuntimeMeshCook", usdRuntimeMeshes.diagnostics},
                {"usdRuntimeSceneEntities", usdSceneEntities.diagnostics},
                {"counts", {
                    {"prims", usdData.prims.size()},
                    {"meshes", usdData.meshCount},
                    {"nativeMeshes", usdMeshGuids.size()},
                    {"nativeMaterials", usdMaterialGuids.size()},
                    {"materialBindings", usdData.materialBindingCount},
                    {"cameras", usdData.cameraCount},
                    {"runtimeCameras", usdSceneEntities.cameraCount},
                    {"lights", usdData.lightCount},
                    {"runtimeLights", usdSceneEntities.lightCount},
                    {"animations", generatedUsdAnimationAssets.size()},
                    {"animationControllers", generatedUsdAnimationControllerAssets.size()},
                    {"prefabNodes", usdPrefabNodes.size()},
                }},
            };
            cookedPayloads.push_back(runtimePayload);
            placeholder["runtimePayload"] = runtimePayload;
            placeholder["thumbnail"] = thumbnailMetadata;
            placeholder["usdMetadata"] = usdMetadata;
            placeholder["prefab"] = {
                {"version", 1},
                {"guid", guid},
                {"name", name},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"runtimePayload", runtimePayload},
                {"rootNodes", usdPrefabRootNodes},
                {"nodes", usdPrefabNodes},
            };
            placeholder["rootNodes"] = usdPrefabRootNodes;
            placeholder["nodes"] = usdPrefabNodes;
            placeholder["sourceHierarchy"] = usdData.prims;
            placeholder["rootPrims"] = usdData.rootPrims;
            placeholder["prefabNodeCount"] = usdPrefabNodes.size();
            placeholder["primCount"] = usdData.prims.size();
            placeholder["meshCount"] = usdData.meshCount;
            placeholder["nativeMeshCount"] = usdMeshGuids.size();
            placeholder["meshAssets"] = usdMeshAssets;
            placeholder["nativeMaterialCount"] = usdMaterialGuids.size();
            placeholder["materialAssets"] = usdMaterialAssets;
            placeholder["materialDeduplication"] = usdMaterialDeduplication;
            placeholder["materialBindingCount"] = usdData.materialBindingCount;
            placeholder["cameraCount"] = usdData.cameraCount;
            placeholder["lightCount"] = usdData.lightCount;
            placeholder["runtimeCameras"] = usdSceneEntities.cameras;
            placeholder["runtimeLights"] = usdSceneEntities.lights;
            placeholder["animationAssets"] = generatedUsdAnimationAssets;
            placeholder["animationControllerAssets"] = generatedUsdAnimationControllerAssets;
            placeholder["runtimeTransformAnimationPlaybackImplemented"] = !generatedUsdAnimationAssets.empty() && !generatedUsdAnimationControllerAssets.empty();
            placeholder["animationControllerBindingImplemented"] = !generatedUsdAnimationControllerAssets.empty();
            placeholder["runtimeMeshPointAnimationPlaybackImplemented"] = decodedMeshPointChannelCount > 0 && !generatedUsdAnimationControllerAssets.empty();
            placeholder["runtimeCameraLightParameterPlaybackImplemented"] = usdData.decodedCameraLightAnimationChannelCount > 0 && !generatedUsdAnimationControllerAssets.empty();
            placeholder["decodedMeshPointAnimationChannelCount"] = decodedMeshPointChannelCount;
            placeholder["decodedCameraLightAnimationChannelCount"] = usdData.decodedCameraLightAnimationChannelCount;
            placeholder["cameraRuntimeConversionImplemented"] = usdSceneEntities.cameraCount > 0;
            placeholder["lightRuntimeConversionImplemented"] = usdSceneEntities.lightCount > 0;
            placeholder["sceneGraphMetadataImportImplemented"] = true;
            placeholder["meshNativeCookImplemented"] = !usdMeshGuids.empty();
            placeholder["usdRuntimeMeshCook"] = usdRuntimeMeshes.diagnostics;
            placeholder["materialNativeCookImplemented"] = !usdMaterialGuids.empty();
            placeholder["materialBindingCookImplemented"] = !usdMaterialGuids.empty();
            placeholder["shaderNetworkConversionImplemented"] = usdShaderNetworkConverted;
            placeholder["textureReferenceExtractionImplemented"] = usdTextureReferenceExtractionImplemented;
            placeholder["textureReferenceCount"] = usdTextureReferenceCount;
            placeholder["unsupportedShaderNodeCount"] = usdUnsupportedShaderNodeCount;
            placeholder["unsupportedShaderNodes"] = usdUnsupportedShaderNodes;
            placeholder["shaderTextureMaterialBindingImplemented"] = usdShaderTextureMaterialBindingImplemented;
            placeholder["shaderTextureBindingCount"] = usdShaderTextureBindingCount;
            placeholder["usdExternalTextureNativeCookImplemented"] = usdExternalTextureNativeCookImplemented;
            placeholder["usdExternalTextureCookCount"] = usdExternalTextureGuids.size();
            placeholder["usdExternalTextureSingleFlightReuseCount"] = usdExternalTextureSingleFlightReuseCount;
            placeholder["usdExternalTexturePersistentCacheHitCount"] = usdExternalTexturePersistentCacheHitCount;
            placeholder["usdExternalTextureAssets"] = usdExternalTextureAssets;
            placeholder["textureNativeCookImplemented"] = usdShaderTextureMaterialBindingImplemented || usdzTextureNativeCookImplemented || usdExternalTextureNativeCookImplemented;
            placeholder["usdRuntimeSceneEntities"] = usdSceneEntities.diagnostics;
            placeholder["usdzTextureProvenanceInspectionImplemented"] = usdzPackageTextures.inspected;
            placeholder["usdzPackagedTextureEntryCount"] = usdzPackageTextures.textureEntryCount;
            placeholder["usdzTextureExtractionImplemented"] = usdzTextureExtractionImplemented;
            placeholder["usdzTextureNativeCookImplemented"] = usdzTextureNativeCookImplemented;
            placeholder["usdzExtractedTextureCount"] = usdzExtractedTextureCount;
            placeholder["usdzNativeTextureCookCount"] = usdzNativeTextureCookCount;
            placeholder["usdzTexturePersistentCacheHitCount"] = usdzTexturePersistentCacheHitCount;
            placeholder["usdzTextureAssets"] = usdzTextureAssets;
            placeholder["usdzPackageTextures"] = usdzPackageTextures.diagnostics;
            placeholder["viewportPlacementImplemented"] = true;
            placeholder["runtimePackagePlacementImplemented"] = true;
            placeholder["runtimeReloadParityImplemented"] = true;
            placeholder["runtimePlacementPolicy"] = runtimePayload.value("runtimePlacementPolicy", std::string{});
            placeholder["sourceExtension"] = sourceExtension;
            placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
            placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
            cache["runtimePayload"] = runtimePayload;
            cache["cookedPayloads"] = cookedPayloads;
            cache["thumbnail"] = thumbnailMetadata;
            cache["usdMetadata"] = usdMetadata;
            cache["prefab"] = placeholder["prefab"];
            cache["rootNodes"] = usdPrefabRootNodes;
            cache["nodes"] = usdPrefabNodes;
            cache["prefabNodeCount"] = usdPrefabNodes.size();
            cache["primCount"] = usdData.prims.size();
            cache["meshCount"] = usdData.meshCount;
            cache["nativeMeshCount"] = usdMeshGuids.size();
            cache["meshAssets"] = usdMeshAssets;
            cache["nativeMaterialCount"] = usdMaterialGuids.size();
            cache["materialAssets"] = usdMaterialAssets;
            cache["materialDeduplication"] = usdMaterialDeduplication;
            cache["textureReferenceExtractionImplemented"] = usdTextureReferenceExtractionImplemented;
            cache["textureReferenceCount"] = usdTextureReferenceCount;
            cache["unsupportedShaderNodeCount"] = usdUnsupportedShaderNodeCount;
            cache["unsupportedShaderNodes"] = usdUnsupportedShaderNodes;
            cache["shaderTextureMaterialBindingImplemented"] = usdShaderTextureMaterialBindingImplemented;
            cache["shaderTextureBindingCount"] = usdShaderTextureBindingCount;
            cache["usdExternalTextureNativeCookImplemented"] = usdExternalTextureNativeCookImplemented;
            cache["usdExternalTextureCookCount"] = usdExternalTextureGuids.size();
            cache["usdExternalTextureSingleFlightReuseCount"] = usdExternalTextureSingleFlightReuseCount;
            cache["usdExternalTexturePersistentCacheHitCount"] = usdExternalTexturePersistentCacheHitCount;
            cache["usdExternalTextureAssets"] = usdExternalTextureAssets;
            cache["textureNativeCookImplemented"] = usdShaderTextureMaterialBindingImplemented || usdzTextureNativeCookImplemented || usdExternalTextureNativeCookImplemented;
            cache["usdzTextureProvenanceInspectionImplemented"] = usdzPackageTextures.inspected;
            cache["usdzPackagedTextureEntryCount"] = usdzPackageTextures.textureEntryCount;
            cache["usdzTextureExtractionImplemented"] = usdzTextureExtractionImplemented;
            cache["usdzTextureNativeCookImplemented"] = usdzTextureNativeCookImplemented;
            cache["usdzExtractedTextureCount"] = usdzExtractedTextureCount;
            cache["usdzNativeTextureCookCount"] = usdzNativeTextureCookCount;
            cache["usdzTexturePersistentCacheHitCount"] = usdzTexturePersistentCacheHitCount;
            cache["usdzTextureAssets"] = usdzTextureAssets;
            cache["usdzPackageTextures"] = usdzPackageTextures.diagnostics;
            cache["materialBindingCount"] = usdData.materialBindingCount;
            cache["cameraCount"] = usdData.cameraCount;
            cache["lightCount"] = usdData.lightCount;
            cache["runtimeCameraCount"] = usdSceneEntities.cameraCount;
            cache["runtimeLightCount"] = usdSceneEntities.lightCount;
            cache["runtimeCameras"] = usdSceneEntities.cameras;
            cache["runtimeLights"] = usdSceneEntities.lights;
            cache["animationAssets"] = generatedUsdAnimationAssets;
            cache["animationControllerAssets"] = generatedUsdAnimationControllerAssets;
            cache["runtimeTransformAnimationPlaybackImplemented"] = !generatedUsdAnimationAssets.empty() && !generatedUsdAnimationControllerAssets.empty();
            cache["animationControllerBindingImplemented"] = !generatedUsdAnimationControllerAssets.empty();
            cache["runtimeMeshPointAnimationPlaybackImplemented"] = decodedMeshPointChannelCount > 0 && !generatedUsdAnimationControllerAssets.empty();
            cache["runtimeCameraLightParameterPlaybackImplemented"] = usdData.decodedCameraLightAnimationChannelCount > 0 && !generatedUsdAnimationControllerAssets.empty();
            cache["decodedMeshPointAnimationChannelCount"] = decodedMeshPointChannelCount;
            cache["decodedCameraLightAnimationChannelCount"] = usdData.decodedCameraLightAnimationChannelCount;
            cache["viewportPlacementImplemented"] = true;
            cache["runtimePackagePlacementImplemented"] = true;
            cache["runtimeReloadParityImplemented"] = true;
            cache["runtimePlacementPolicy"] = runtimePayload.value("runtimePlacementPolicy", std::string{});
        } else {
            importFailed = true;
            for (const std::string& error : usdData.errors) {
                result.warnings.push_back(error);
            }
            runtimePayload = {
                {"kind", "UsdStageImportUnavailable"},
                {"sourcePath", effectiveSourceString},
                {"available", false},
                {"sceneGraphMetadataImportImplemented", false},
                {"meshNativeCookImplemented", false},
                {"materialNativeCookImplemented", false},
                {"usdzTextureProvenanceInspectionImplemented", usdzPackageTextures.inspected},
                {"usdzPackagedTextureEntryCount", usdzPackageTextures.textureEntryCount},
                {"usdzTextureExtractionImplemented", usdzTextureExtractionImplemented},
                {"usdzTextureNativeCookImplemented", usdzTextureNativeCookImplemented},
                {"usdzExtractedTextureCount", usdzExtractedTextureCount},
                {"usdzNativeTextureCookCount", usdzNativeTextureCookCount},
                {"usdzTextureAssets", usdzTextureAssets},
                {"usdzPackageTextures", usdzPackageTextures.diagnostics},
                {"viewportPlacementImplemented", false},
                {"disabledReason", "RTV_ENABLE_OPENUSD_IMPORTER=OFF or OpenUSD unavailable"},
                {"usdStageImport", usdData.diagnostics},
            };
            cookedPayloads.push_back(runtimePayload);
            placeholder["runtimePayload"] = runtimePayload;
            placeholder["thumbnail"] = thumbnailMetadata;
            placeholder["usdMetadata"] = usdMetadata;
            placeholder["sceneGraphMetadataImportImplemented"] = false;
            placeholder["meshNativeCookImplemented"] = false;
            placeholder["materialNativeCookImplemented"] = false;
            placeholder["usdzTextureProvenanceInspectionImplemented"] = usdzPackageTextures.inspected;
            placeholder["usdzPackagedTextureEntryCount"] = usdzPackageTextures.textureEntryCount;
            placeholder["usdzTextureExtractionImplemented"] = usdzTextureExtractionImplemented;
            placeholder["usdzTextureNativeCookImplemented"] = usdzTextureNativeCookImplemented;
            placeholder["usdzExtractedTextureCount"] = usdzExtractedTextureCount;
            placeholder["usdzNativeTextureCookCount"] = usdzNativeTextureCookCount;
            placeholder["usdzTextureAssets"] = usdzTextureAssets;
            placeholder["usdzPackageTextures"] = usdzPackageTextures.diagnostics;
            placeholder["viewportPlacementImplemented"] = false;
            cache["runtimePayload"] = runtimePayload;
            cache["cookedPayloads"] = cookedPayloads;
            cache["usdMetadata"] = usdMetadata;
            cache["usdzTextureProvenanceInspectionImplemented"] = usdzPackageTextures.inspected;
            cache["usdzPackagedTextureEntryCount"] = usdzPackageTextures.textureEntryCount;
            cache["usdzTextureExtractionImplemented"] = usdzTextureExtractionImplemented;
            cache["usdzTextureNativeCookImplemented"] = usdzTextureNativeCookImplemented;
            cache["usdzExtractedTextureCount"] = usdzExtractedTextureCount;
            cache["usdzNativeTextureCookCount"] = usdzNativeTextureCookCount;
            cache["usdzTextureAssets"] = usdzTextureAssets;
            cache["usdzPackageTextures"] = usdzPackageTextures.diagnostics;
        }
    } else if (sourceIsObj) {
        const auto inspectStart = std::chrono::steady_clock::now();
        setProgress(0.45f, "Inspecting OBJ source");
        objMetadata = inspectObjSource(effectiveSourcePath, result.warnings);
        result.workerInspectMs = elapsedMilliseconds(inspectStart);
        rootThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(effectiveSourcePath, workspace.root) : std::string{};
        thumbnailMetadata = thumbnailMetadataJson("GeneratedSourcePreview", rootThumbnailPath, sourceHash, importSettingsHash);

        setProgress(0.62f, "Cooking OBJ mesh payload");
        ObjRuntimeMeshCookData objCookData = loadObjRuntimeMesh(effectiveSourcePath, name);
        for (const std::string& warning : objCookData.warnings) {
            result.warnings.push_back(warning);
        }
        if (objCookData.supported && objCookData.errors.empty()) {
            nlohmann::json linkedMtlLibraries = nlohmann::json::array();
            nlohmann::json objMtlMaterialAssets = nlohmann::json::array();
            std::vector<AssetGuid> objMaterialGuids;
            std::unordered_map<std::string, nlohmann::json> mtlMaterialsByName;
            if (request.settings.importMaterials && objMetadata.contains("materialLibraries") && objMetadata["materialLibraries"].is_array()) {
                for (const nlohmann::json& item : objMetadata["materialLibraries"]) {
                    if (!item.is_string()) {
                        continue;
                    }
                    const std::string library = item.get<std::string>();
                    const std::filesystem::path relativeLibraryPath = std::filesystem::path(library).lexically_normal();
                    if (relativeLibraryPath.is_absolute()) {
                        result.warnings.push_back("Skipped absolute OBJ material library during runtime material cook: " + library);
                        continue;
                    }
                    const std::filesystem::path mtlPath = (effectiveSourcePath.parent_path() / relativeLibraryPath).lexically_normal();
                    nlohmann::json linkedMtlMetadata = inspectMtlSource(mtlPath, result.warnings);
                    linkedMtlLibraries.push_back({
                        {"path", mtlPath.generic_string()},
                        {"sourceToken", library},
                        {"inspected", linkedMtlMetadata.value("inspected", false)},
                        {"materialCount", linkedMtlMetadata.value("materialCount", 0u)},
                    });
                    if (linkedMtlMetadata.contains("materials") && linkedMtlMetadata["materials"].is_array()) {
                        for (const nlohmann::json& materialJson : linkedMtlMetadata["materials"]) {
                            const std::string materialName = materialJson.value("name", std::string{});
                            if (!materialName.empty() && mtlMaterialsByName.find(materialName) == mtlMaterialsByName.end()) {
                                nlohmann::json materialWithSource = materialJson;
                                materialWithSource["__sourceMtlPath"] = mtlPath.generic_string();
                                mtlMaterialsByName.emplace(materialName, std::move(materialWithSource));
                            }
                        }
                    }
                }
            }

            std::vector<AssetGuid> objTextureGuids;
            std::unordered_map<std::string, size_t> objTextureIndexByKey;
            nlohmann::json objMtlTextureAssets = nlohmann::json::array();
            size_t objTexturePersistentCacheHitCount = 0;
            std::unordered_map<std::string, CookedAssetReuseEntry> objCookedMaterialsByContent;
            nlohmann::json objMaterialDedupAliases = nlohmann::json::array();
            size_t objMaterialContentReuseCount = 0;
            size_t objMaterialPersistentCacheHitCount = 0;

            auto cookObjMtlTexture = [&](const std::filesystem::path& mtlSourcePath, const std::string& textureToken, const std::string& role, const std::string& materialName, const std::string& mtlKey) -> std::optional<TextureAssetHandle> {
                if (!request.settings.importTextures || textureToken.empty() || mtlSourcePath.empty()) {
                    return std::nullopt;
                }
                if (textureToken.find("://") != std::string::npos || isDataUri(textureToken)) {
                    result.warnings.push_back("Skipped unsupported OBJ-linked MTL texture URI for " + materialName + ": " + textureToken);
                    return std::nullopt;
                }
                const std::filesystem::path relativeTexturePath = std::filesystem::path(textureToken).lexically_normal();
                if (relativeTexturePath.is_absolute()) {
                    result.warnings.push_back("Skipped absolute OBJ-linked MTL texture path for " + materialName + ": " + textureToken);
                    return std::nullopt;
                }
                const std::filesystem::path textureSourcePath = (mtlSourcePath.parent_path() / relativeTexturePath).lexically_normal();
                const NativeTextureRole nativeRole = nativeTextureRoleFromString(role);
                const NativeTextureColorSpace colorSpace = mtlTextureColorSpaceForRole(nativeRole);
                const std::string textureKey = nativeTextureRoleName(nativeRole) + ":" + textureSourcePath.generic_string();
                const auto existing = objTextureIndexByKey.find(textureKey);
                if (existing != objTextureIndexByKey.end()) {
                    return TextureAssetHandle{static_cast<uint32_t>(existing->second)};
                }
                if (!std::filesystem::exists(textureSourcePath)) {
                    result.warnings.push_back("OBJ-linked MTL texture reference was not found for " + materialName + ": " + textureSourcePath.string());
                    return std::nullopt;
                }

                TextureData textureData;
                try {
                    textureData = TextureLoader::load(textureSourcePath.string(), importTextureFormatSupport, nativeRole, colorSpace);
                } catch (const std::exception& ex) {
                    result.warnings.push_back("OBJ-linked MTL texture decode failed for " + textureSourcePath.string() + ": " + ex.what());
                    return std::nullopt;
                }

                const size_t textureIndex = objTextureGuids.size();
                const AssetGuid textureGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "ObjMtlTexture", textureIndex);
                const std::string textureName = safeStem(textureSourcePath.stem().string() + "_" + nativeTextureRoleName(nativeRole));
                const std::filesystem::path texturePath = importedDir / "Textures" / (textureName + ".rttexture.json");
                const std::filesystem::path nativeTexturePath = cacheDir / "Textures" / (textureName + ".rttexture");
                std::filesystem::create_directories(texturePath.parent_path(), ec);
                std::filesystem::create_directories(nativeTexturePath.parent_path(), ec);

                TextureAsset textureAsset;
                textureAsset.name = textureName;
                textureAsset.sourcePath = textureSourcePath;
                textureAsset.width = static_cast<uint32_t>(std::max(0, textureData.width));
                textureAsset.height = static_cast<uint32_t>(std::max(0, textureData.height));
                textureAsset.channels = 4;
                textureAsset.sourceArrayLayers = textureData.sourceArrayLayers;
                textureAsset.sourceDepth = textureData.sourceDepth;
                textureAsset.sourceFaceCount = textureData.sourceFaceCount;
                textureAsset.sourceIsCubemap = textureData.sourceIsCubemap;
                textureAsset.mipLevels = std::max(1, textureData.mipLevels);
                textureAsset.srgb = colorSpace == NativeTextureColorSpace::Srgb;
                textureAsset.linearColorSpace = colorSpace != NativeTextureColorSpace::Srgb || textureData.linearColorSpace;
                textureAsset.isCompressed = textureData.isCompressed;
                textureAsset.format = textureData.format;
                textureAsset.compressedFormat = textureData.compressedFormat;
                textureAsset.sourceContainerKind = textureData.sourceContainerKind;
                textureAsset.nativePayloadSource = textureData.nativePayloadSource;
                textureAsset.sourceContainerPreserved = textureData.sourceContainerPreserved;
                textureAsset.sourceContainerTranscoded = textureData.sourceContainerTranscoded;
                textureAsset.rgba8 = textureData.pixels;
                textureAsset.mipData = textureData.mipData;

                const std::optional<NativeAssetCookResult> reusableTexture =
                    reusableNativeCookResult(nativeTexturePath, NativeAssetKind::Texture, textureGuid, textureSourcePath);
                const bool texturePersistentCacheReused = reusableTexture.has_value();
                if (texturePersistentCacheReused) {
                    ++objTexturePersistentCacheHitCount;
                }
                const NativeAssetCookResult textureCook = reusableTexture.has_value()
                    ? *reusableTexture
                    : nativeCooker.cookTexture(
                        nativeCookInput(textureGuid, nativeTexturePath, textureName),
                        textureAsset,
                        nativeTextureRoleName(nativeRole));
                if (!recordNativeCookResult(textureCook, textureName)) {
                    return std::nullopt;
                }
                objTextureGuids.push_back(textureGuid);
                objTextureIndexByKey.emplace(textureKey, textureIndex);
                nlohmann::json textureRole = {
                    {"role", nativeTextureRoleName(nativeRole)},
                    {"source", "objLinkedMtlTextureMap"},
                    {"mtlKey", mtlKey},
                    {"material", materialName},
                    {"confidence", "authored-slot"},
                };
                nlohmann::json texturePayload = nativeCookRuntimePayloadJson(
                    textureCook,
                    NativeAssetKind::Texture,
                    textureGuid,
                    workspace.root,
                    effectiveSourcePath,
                    sourceHash,
                    importSettingsHash);
                texturePayload["assetIndex"] = textureIndex;
                texturePayload["sourceTexturePath"] = textureSourcePath.generic_string();
                texturePayload["sourceMtlPath"] = mtlSourcePath.generic_string();
                texturePayload["textureRole"] = textureRole;
                texturePayload["persistentCacheReused"] = texturePersistentCacheReused;
                cookedPayloads.push_back(texturePayload);
                rootDependencies.push_back(textureGuid);
                (void)writeJson(texturePath, {
                    {"version", 1},
                    {"kind", "ImportedObjMtlTexture"},
                    {"guid", textureGuid},
                    {"sourcePath", textureSourcePath.generic_string()},
                    {"rootSourcePath", effectiveSourceString},
                    {"sourceMtlPath", mtlSourcePath.generic_string()},
                    {"originalRootSourcePath", originalSourceString},
                    {"copiedRootSourcePath", copiedSourceString},
                    {"sourceHash", sourceHash},
                    {"importSettingsHash", importSettingsHash},
                    {"runtimePayload", texturePayload},
                    {"width", textureAsset.width},
                    {"height", textureAsset.height},
                    {"channels", textureAsset.channels},
                    {"colorSpace", textureColorSpaceLabel(textureAsset)},
                    {"textureRole", textureRole},
                    {"mtl", {{"material", materialName}, {"key", mtlKey}, {"token", textureToken}}},
                });

                AssetRecord textureRecord;
                textureRecord.guid = textureGuid;
                textureRecord.type = AssetType::Texture;
                textureRecord.displayName = textureName;
                textureRecord.sourcePath = textureSourcePath.generic_string();
                textureRecord.importedPath = genericRelativeOrValue(texturePath, workspace.root);
                textureRecord.cachePath = texturePayload.value("cachePath", std::string{});
                textureRecord.sourceHash = sourceHash;
                textureRecord.importSettingsHash = importSettingsHash;
                textureRecord.lastModifiedTimestamp = timestampString();
                textureRecord.importSettings = request.settings;
                textureRecord.status = AssetImportStatus::Imported;
                records.push_back(std::move(textureRecord));

                objMtlTextureAssets.push_back({
                    {"guid", textureGuid},
                    {"name", textureName},
                    {"sourcePath", textureSourcePath.generic_string()},
                    {"sourceMtlPath", mtlSourcePath.generic_string()},
                    {"path", genericRelativeOrValue(texturePath, workspace.root)},
                    {"role", nativeTextureRoleName(nativeRole)},
                    {"mtlKey", mtlKey},
                });
                return TextureAssetHandle{static_cast<uint32_t>(textureIndex)};
            };

            auto assignObjMtlTexture = [&](MaterialAsset& material, const std::filesystem::path& mtlSourcePath, const nlohmann::json& textureMaps, const nlohmann::json& textureMapOptions, const char* mtlKey, nlohmann::json& textureDependencies, nlohmann::json& unboundTextures) {
                const nlohmann::json* mapValue = jsonObjectValueCaseInsensitive(textureMaps, mtlKey);
                if (mapValue == nullptr || !mapValue->is_string()) {
                    return;
                }
                const std::string textureToken = mtlTexturePathValueToken(mapValue->get<std::string>());
                const std::string role = mtlTextureRoleStringForKey(mtlKey);
                const nlohmann::json options = mtlTextureMapOptionsForKey(textureMapOptions, mtlKey);
                const nlohmann::json optionDiagnostics = mtlTextureMapOptionRuntimeDiagnostics(options, mtlKey);
                std::optional<TextureAssetHandle> handle = cookObjMtlTexture(mtlSourcePath, textureToken, role, material.name, mtlKey);
                if (!handle.has_value()) {
                    return;
                }
                const size_t textureIndex = handle->index;
                const AssetGuid textureGuid = textureIndex < objTextureGuids.size() ? objTextureGuids[textureIndex] : std::string{};
                const std::string normalizedKey = lowerString(mtlKey);
                bool bound = true;
                if (normalizedKey == "map_kd") {
                    material.baseColorTexture = *handle;
                } else if (normalizedKey == "map_ke") {
                    material.emissiveTexture = *handle;
                } else if (normalizedKey == "map_bump" || normalizedKey == "bump") {
                    material.normalTexture = *handle;
                } else if (normalizedKey == "map_ks") {
                    material.hasSpecular = 1u;
                    material.specularColorTexture = *handle;
                } else if (normalizedKey == "map_pr" || normalizedKey == "map_pm" || normalizedKey == "map_ns") {
                    material.metallicRoughnessTexture = *handle;
                } else if (normalizedKey == std::string({'m', 'a', 'p', '_', 'd'})) {
                    material.opacityTexture = *handle;
                } else if (normalizedKey == std::string({'d', 'i', 's', 'p'})) {
                    material.heightTexture = *handle;
                    material.heightScale = std::clamp(mtlTextureOptionFloat(mapValue->get<std::string>(), "-bm", material.heightScale), 0.0f, 0.25f);
                } else {
                    bound = false;
                }
                if (bound) {
                    textureDependencies.push_back({
                        {"guid", textureGuid},
                        {"role", role},
                        {"mtlKey", mtlKey},
                        {"textureMapOptions", options},
                        {"textureMapOptionDiagnostics", optionDiagnostics},
                    });
                } else {
                    unboundTextures.push_back({
                        {"guid", textureGuid},
                        {"role", role},
                        {"mtlKey", mtlKey},
                        {"textureMapOptions", options},
                        {"textureMapOptionDiagnostics", optionDiagnostics},
                        {"reason", "native-material-slot-not-yet-defined"},
                    });
                }
            };

            if (request.settings.importMaterials && objCookData.diagnostics.contains("materials") && objCookData.diagnostics["materials"].is_array()) {
                for (const nlohmann::json& materialReport : objCookData.diagnostics["materials"]) {
                    const size_t materialId = materialReport.value("id", objMaterialGuids.size());
                    const std::string authoredMaterialName = materialReport.value("name", std::string{});
                    const std::string materialName = safeStem(authoredMaterialName.empty() ? ("ObjMaterial_" + std::to_string(materialId)) : authoredMaterialName);
                    const auto materialIt = mtlMaterialsByName.find(authoredMaterialName);
                    MaterialAsset material = materialIt != mtlMaterialsByName.end()
                        ? mtlMaterialAssetFromMetadata(materialIt->second, materialName)
                        : MaterialAsset{};
                    material.name = materialName;
                    if (materialIt == mtlMaterialsByName.end()) {
                        result.warnings.push_back("OBJ material '" + authoredMaterialName + "' did not resolve to an MTL material; emitted a default native material slot.");
                    }
                    nlohmann::json textureDependencies = nlohmann::json::array();
                    nlohmann::json unboundTextures = nlohmann::json::array();
                    if (materialIt != mtlMaterialsByName.end()) {
                        const std::filesystem::path mtlSourcePath = materialIt->second.value("__sourceMtlPath", std::string{});
                        const nlohmann::json emptyTextureMaps = nlohmann::json::object();
                        const nlohmann::json& textureMaps = materialIt->second.contains("textureMaps") ? materialIt->second["textureMaps"] : emptyTextureMaps;
                        const nlohmann::json& textureMapOptions = materialIt->second.contains("textureMapOptions") ? materialIt->second["textureMapOptions"] : emptyTextureMaps;
                        for (const char* key : {"map_Kd", "map_Ke", "map_Ks", "map_Ns", "map_Pr", "map_Pm", "map_bump", "bump", "map_d", "disp"}) {
                            assignObjMtlTexture(material, mtlSourcePath, textureMaps, textureMapOptions, key, textureDependencies, unboundTextures);
                        }
                    }
                    const AssetGuid materialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "ObjMtlMaterial", materialId);
                    if (materialId >= objMaterialGuids.size()) {
                        objMaterialGuids.resize(materialId + 1u);
                    }
                    const std::string materialContentKey = materialContentDedupKey(material, objTextureGuids);
                    const auto existingMaterial = objCookedMaterialsByContent.find(materialContentKey);
                    if (existingMaterial != objCookedMaterialsByContent.end()) {
                        objMaterialGuids[materialId] = existingMaterial->second.guid;
                        ++objMaterialContentReuseCount;
                        objMaterialDedupAliases.push_back({
                            {"sourceMaterialName", authoredMaterialName},
                            {"materialId", materialId},
                            {"sourceName", materialName},
                            {"reusedSourceIndex", existingMaterial->second.sourceIndex},
                            {"reusedName", existingMaterial->second.name},
                            {"guid", existingMaterial->second.guid},
                            {"nativePath", genericRelativeOrValue(existingMaterial->second.nativePath, workspace.root)},
                        });
                        continue;
                    }
                    objMaterialGuids[materialId] = materialGuid;
                    rootDependencies.push_back(materialGuid);
                    const std::filesystem::path materialPath = importedDir / "Materials" / (materialName + ".rtmaterial.json");
                    const std::filesystem::path nativeMaterialPath = cacheDir / "Materials" / (materialName + ".rtmaterial");
                    std::filesystem::create_directories(materialPath.parent_path(), ec);
                    std::filesystem::create_directories(nativeMaterialPath.parent_path(), ec);
                    const std::filesystem::path materialCacheDependency = materialIt != mtlMaterialsByName.end()
                        ? std::filesystem::path(materialIt->second.value("__sourceMtlPath", effectiveSourceString))
                        : effectiveSourcePath;
                    const std::optional<NativeAssetCookResult> reusableMaterial =
                        reusableNativeCookResult(nativeMaterialPath, NativeAssetKind::Material, materialGuid, materialCacheDependency);
                    const bool materialPersistentCacheReused = reusableMaterial.has_value();
                    if (materialPersistentCacheReused) {
                        ++objMaterialPersistentCacheHitCount;
                    }
                    const NativeAssetCookResult materialCook = reusableMaterial.has_value()
                        ? *reusableMaterial
                        : nativeCooker.cookMaterial(
                            nativeCookInput(materialGuid, nativeMaterialPath, materialName),
                            material,
                            objTextureGuids);
                    if (!recordNativeCookResult(materialCook, materialName)) {
                        result.workerTotalMs = elapsedMilliseconds(workerStart);
                        return result;
                    }
                    nlohmann::json materialPayload = nativeCookRuntimePayloadJson(
                        materialCook,
                        NativeAssetKind::Material,
                        materialGuid,
                        workspace.root,
                        effectiveSourcePath,
                        sourceHash,
                        importSettingsHash);
                    materialPayload["assetIndex"] = materialId;
                    materialPayload["sourceMaterialName"] = authoredMaterialName;
                    materialPayload["textureDependencyCount"] = textureDependencies.size();
                    materialPayload["persistentCacheReused"] = materialPersistentCacheReused;
                    materialPayload["contentDedupKey"] = materialContentKey;
                    objCookedMaterialsByContent.emplace(materialContentKey, CookedAssetReuseEntry{
                        .guid = materialGuid,
                        .name = materialName,
                        .importedPath = materialPath,
                        .nativePath = nativeMaterialPath,
                        .sourceIndex = materialId,
                    });
                    cookedPayloads.push_back(materialPayload);
                    nlohmann::json pbrMetadata = materialPbrMetadataJson(material);
                    pbrMetadata["workflow"] = "OBJ/MTL Phong-to-PBR";
                    (void)writeJson(materialPath, {
                        {"version", 1},
                        {"kind", "ImportedObjMtlMaterial"},
                        {"guid", materialGuid},
                        {"sourcePath", effectiveSourceString},
                        {"originalSourcePath", originalSourceString},
                        {"copiedSourcePath", copiedSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"runtimePayload", materialPayload},
                        {"sourceMaterialName", authoredMaterialName},
                        {"materialId", materialId},
                        {"alphaMode", materialAlphaModeLabel(material.alphaMode)},
                        {"pbr", pbrMetadata},
                        {"mtl", materialIt != mtlMaterialsByName.end() ? materialIt->second : nlohmann::json::object()},
                        {"textureDependencies", textureDependencies},
                        {"unboundTextureMaps", unboundTextures},
                        {"objMaterialSlotBinding", {{"implemented", true}, {"slot", materialId}}},
                    });

                    AssetRecord materialRecord;
                    materialRecord.guid = materialGuid;
                    materialRecord.type = AssetType::Material;
                    materialRecord.displayName = materialName;
                    materialRecord.sourcePath = effectiveSourceString;
                    materialRecord.importedPath = genericRelativeOrValue(materialPath, workspace.root);
                    materialRecord.cachePath = materialPayload.value("cachePath", std::string{});
                    materialRecord.sourceHash = sourceHash;
                    materialRecord.importSettingsHash = importSettingsHash;
                    materialRecord.lastModifiedTimestamp = timestampString();
                    materialRecord.importSettings = request.settings;
                    materialRecord.status = AssetImportStatus::Imported;
                    for (const auto& dep : textureDependencies) {
                        materialRecord.dependencies.push_back(AssetDependency{dep.value("guid", std::string{}), dep.value("role", std::string{})});
                    }
                    records.push_back(std::move(materialRecord));

                    objMtlMaterialAssets.push_back({
                        {"guid", materialGuid},
                        {"name", materialName},
                        {"sourceMaterialName", authoredMaterialName},
                        {"materialId", materialId},
                        {"path", genericRelativeOrValue(materialPath, workspace.root)},
                        {"mtlResolved", materialIt != mtlMaterialsByName.end()},
                        {"textureDependencyCount", textureDependencies.size()},
                        {"unboundTextureMapCount", unboundTextures.size()},
                    });
                }
            }

            MeshAsset objMeshForCook = objCookData.mesh;
            for (MeshPrimitiveAsset& primitive : objMeshForCook.primitives) {
                if (!primitive.material.valid() || primitive.material.index >= objMaterialGuids.size() || objMaterialGuids[primitive.material.index].empty()) {
                    primitive.material = MaterialAssetHandle{};
                }
            }
            const std::filesystem::path meshPath = importedDir / "Meshes" / (name + ".rtmesh.json");
            const std::filesystem::path nativeMeshPath = cacheDir / "Meshes" / (name + ".rtmesh");
            std::filesystem::create_directories(meshPath.parent_path(), ec);
            std::filesystem::create_directories(nativeMeshPath.parent_path(), ec);
            const std::optional<NativeAssetCookResult> reusableMesh =
                reusableNativeCookResult(nativeMeshPath, NativeAssetKind::Mesh, guid, effectiveSourcePath);
            const bool objMeshPersistentCacheReused = reusableMesh.has_value();
            const NativeAssetCookResult meshCook = reusableMesh.has_value()
                ? *reusableMesh
                : nativeCooker.cookMesh(
                    nativeCookInput(guid, nativeMeshPath, name),
                    objMeshForCook,
                    objMaterialGuids,
                    request.settings.buildBlasCache);
            if (!recordNativeCookResult(meshCook, name)) {
                result.workerTotalMs = elapsedMilliseconds(workerStart);
                return result;
            }
            runtimePayload = nativeCookRuntimePayloadJson(
                meshCook,
                NativeAssetKind::Mesh,
                guid,
                workspace.root,
                effectiveSourcePath,
                sourceHash,
                importSettingsHash);
            runtimePayload["kind"] = "ObjRuntimeMeshPayload";
            runtimePayload["runtimeGeometryCooked"] = true;
            runtimePayload["persistentCacheReused"] = objMeshPersistentCacheReused;
            runtimePayload["objMaterialSlotGuidBindingImplemented"] = !objMaterialGuids.empty();
            runtimePayload["objLinkedMtlTextureBindingImplemented"] = !objTextureGuids.empty();
            runtimePayload["materialSlotCount"] = objMaterialGuids.size();
            runtimePayload["materialAssets"] = objMtlMaterialAssets;
            runtimePayload["textureAssets"] = objMtlTextureAssets;
            runtimePayload["textureCount"] = objTextureGuids.size();
            runtimePayload["texturePersistentCacheHitCount"] = objTexturePersistentCacheHitCount;
            runtimePayload["materialDeduplication"] = {
                {"sourceMaterialSlotCount", objMaterialGuids.size()},
                {"uniqueMaterialCount", objCookedMaterialsByContent.size()},
                {"reusedMaterialCount", objMaterialContentReuseCount},
                {"persistentCacheHitCount", objMaterialPersistentCacheHitCount},
                {"aliases", objMaterialDedupAliases},
            };
            runtimePayload["linkedMtlLibraries"] = linkedMtlLibraries;
            runtimePayload["objRuntimeMeshCook"] = objCookData.diagnostics;
            runtimePayload["objMetadata"] = objMetadata;
            cookedPayloads.push_back(runtimePayload);
            (void)writeJson(meshPath, {
                {"version", 1},
                {"kind", "ImportedObjMesh"},
                {"guid", guid},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"runtimePayload", runtimePayload},
                {"objMetadata", objMetadata},
                {"objRuntimeMeshCook", objCookData.diagnostics},
                {"vertexCount", objMeshForCook.vertices.size()},
                {"indexCount", objMeshForCook.indices.size()},
                {"primitiveCount", objMeshForCook.primitives.size()},
                {"materialAssets", objMtlMaterialAssets},
                {"textureAssets", objMtlTextureAssets},
                {"linkedMtlLibraries", linkedMtlLibraries},
                {"materialBinding", {
                    {"mtlMaterialCreationImplemented", !objMtlMaterialAssets.empty()},
                    {"materialSlotGuidBindingImplemented", !objMaterialGuids.empty()},
                    {"linkedMtlTextureBindingImplemented", !objTextureGuids.empty()},
                    {"viewportPlacementImplemented", true},
                }},
            });
            placeholder["runtimeGeometryCooked"] = true;
            placeholder["objMaterialSlotGuidBindingImplemented"] = !objMaterialGuids.empty();
            placeholder["objLinkedMtlTextureBindingImplemented"] = !objTextureGuids.empty();
            placeholder["materialAssets"] = objMtlMaterialAssets;
            placeholder["textureAssets"] = objMtlTextureAssets;
            placeholder["texturePersistentCacheHitCount"] = objTexturePersistentCacheHitCount;
            placeholder["materialDeduplication"] = runtimePayload["materialDeduplication"];
            placeholder["meshPersistentCacheReused"] = objMeshPersistentCacheReused;
            placeholder["linkedMtlLibraries"] = linkedMtlLibraries;
            placeholder["objRuntimeMeshCook"] = objCookData.diagnostics;
        } else {
            for (const std::string& error : objCookData.errors) {
                result.warnings.push_back(error);
            }
            result.warnings.push_back("OBJ import preserved inspectable mesh/material-library metadata only; enable tinyobjloader-backed runtime cooking for native geometry payloads.");
            runtimePayload = {
                {"kind", "ObjMetadataOnly"},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"available", std::filesystem::exists(effectiveSourcePath)},
                {"validForSource", true},
                {"runtimeGeometryCooked", false},
                {"metadata", objMetadata},
                {"objRuntimeMeshCook", objCookData.diagnostics},
            };
            cookedPayloads.push_back(runtimePayload);
            placeholder["runtimeGeometryCooked"] = false;
            placeholder["objRuntimeMeshCook"] = objCookData.diagnostics;
        }
        placeholder["runtimePayload"] = runtimePayload;
        placeholder["thumbnail"] = thumbnailMetadata;
        placeholder["objMetadata"] = objMetadata;
        collisionLodMetadata = objMetadata.value("collisionLodMetadata", nlohmann::json::object());
        placeholder["collisionLodMetadata"] = collisionLodMetadata;
        placeholder["sourceExtension"] = sourceExtension;
        placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
        placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
        cache["runtimePayload"] = runtimePayload;
        cache["cookedPayloads"] = cookedPayloads;
        cache["thumbnail"] = thumbnailMetadata;
        cache["objMetadata"] = objMetadata;
        cache["collisionLodMetadata"] = collisionLodMetadata;
    } else if (sourceIsMtl) {
        const auto inspectStart = std::chrono::steady_clock::now();
        setProgress(0.45f, "Inspecting MTL source");
        mtlMetadata = inspectMtlSource(effectiveSourcePath, result.warnings);
        result.workerInspectMs = elapsedMilliseconds(inspectStart);
        rootThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(effectiveSourcePath, workspace.root) : std::string{};
        thumbnailMetadata = thumbnailMetadataJson("GeneratedMaterialSourcePreview", rootThumbnailPath, sourceHash, importSettingsHash);

        std::vector<AssetGuid> textureGuids;
        std::unordered_map<std::string, size_t> textureIndexByKey;
        nlohmann::json generatedTextures = nlohmann::json::array();
        nlohmann::json generatedMaterials = nlohmann::json::array();
        size_t mtlTexturePersistentCacheHitCount = 0;
        std::unordered_map<std::string, CookedAssetReuseEntry> mtlCookedMaterialsByContent;
        nlohmann::json mtlMaterialDedupAliases = nlohmann::json::array();
        size_t mtlMaterialContentReuseCount = 0;
        size_t mtlMaterialPersistentCacheHitCount = 0;

        auto cookMtlTexture = [&](const std::string& textureToken, const std::string& role, const std::string& materialName, const std::string& mtlKey) -> std::optional<TextureAssetHandle> {
            if (!request.settings.importTextures || textureToken.empty()) {
                return std::nullopt;
            }
            if (textureToken.find("://") != std::string::npos || isDataUri(textureToken)) {
                result.warnings.push_back("Skipped unsupported MTL texture URI for " + materialName + ": " + textureToken);
                return std::nullopt;
            }
            const std::filesystem::path relativeTexturePath = std::filesystem::path(textureToken).lexically_normal();
            if (relativeTexturePath.is_absolute()) {
                result.warnings.push_back("Skipped absolute MTL texture path for " + materialName + ": " + textureToken);
                return std::nullopt;
            }
            const std::filesystem::path textureSourcePath = (effectiveSourcePath.parent_path() / relativeTexturePath).lexically_normal();
            const NativeTextureRole nativeRole = nativeTextureRoleFromString(role);
            const NativeTextureColorSpace colorSpace = mtlTextureColorSpaceForRole(nativeRole);
            const std::string textureKey = nativeTextureRoleName(nativeRole) + ":" + textureSourcePath.generic_string();
            const auto existing = textureIndexByKey.find(textureKey);
            if (existing != textureIndexByKey.end()) {
                return TextureAssetHandle{static_cast<uint32_t>(existing->second)};
            }
            if (!std::filesystem::exists(textureSourcePath)) {
                result.warnings.push_back("MTL texture reference was not found for " + materialName + ": " + textureSourcePath.string());
                return std::nullopt;
            }

            TextureData textureData;
            try {
                textureData = TextureLoader::load(textureSourcePath.string(), importTextureFormatSupport, nativeRole, colorSpace);
            } catch (const std::exception& ex) {
                result.warnings.push_back("MTL texture decode failed for " + textureSourcePath.string() + ": " + ex.what());
                return std::nullopt;
            }

            const size_t textureIndex = textureGuids.size();
            const AssetGuid textureGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "MtlTexture", textureIndex);
            const std::string textureName = safeStem(textureSourcePath.stem().string() + "_" + nativeTextureRoleName(nativeRole));
            const std::filesystem::path texturePath = importedDir / "Textures" / (textureName + ".rttexture.json");
            const std::filesystem::path nativeTexturePath = cacheDir / "Textures" / (textureName + ".rttexture");
            std::filesystem::create_directories(texturePath.parent_path(), ec);
            std::filesystem::create_directories(nativeTexturePath.parent_path(), ec);

            TextureAsset textureAsset;
            textureAsset.name = textureName;
            textureAsset.sourcePath = textureSourcePath;
            textureAsset.width = static_cast<uint32_t>(std::max(0, textureData.width));
            textureAsset.height = static_cast<uint32_t>(std::max(0, textureData.height));
            textureAsset.channels = 4;
            textureAsset.sourceArrayLayers = textureData.sourceArrayLayers;
            textureAsset.sourceDepth = textureData.sourceDepth;
            textureAsset.sourceFaceCount = textureData.sourceFaceCount;
            textureAsset.sourceIsCubemap = textureData.sourceIsCubemap;
            textureAsset.mipLevels = std::max(1, textureData.mipLevels);
            textureAsset.srgb = colorSpace == NativeTextureColorSpace::Srgb;
            textureAsset.linearColorSpace = colorSpace != NativeTextureColorSpace::Srgb || textureData.linearColorSpace;
            textureAsset.isCompressed = textureData.isCompressed;
            textureAsset.format = textureData.format;
            textureAsset.compressedFormat = textureData.compressedFormat;
            textureAsset.sourceContainerKind = textureData.sourceContainerKind;
            textureAsset.nativePayloadSource = textureData.nativePayloadSource;
            textureAsset.sourceContainerPreserved = textureData.sourceContainerPreserved;
            textureAsset.sourceContainerTranscoded = textureData.sourceContainerTranscoded;
            textureAsset.rgba8 = textureData.pixels;
            textureAsset.mipData = textureData.mipData;

            const std::optional<NativeAssetCookResult> reusableTexture =
                reusableNativeCookResult(nativeTexturePath, NativeAssetKind::Texture, textureGuid, textureSourcePath);
            const bool texturePersistentCacheReused = reusableTexture.has_value();
            if (texturePersistentCacheReused) {
                ++mtlTexturePersistentCacheHitCount;
            }
            const NativeAssetCookResult textureCook = reusableTexture.has_value()
                ? *reusableTexture
                : nativeCooker.cookTexture(
                    nativeCookInput(textureGuid, nativeTexturePath, textureName),
                    textureAsset,
                    nativeTextureRoleName(nativeRole));
            if (!recordNativeCookResult(textureCook, textureName)) {
                return std::nullopt;
            }
            textureGuids.push_back(textureGuid);
            textureIndexByKey.emplace(textureKey, textureIndex);
            nlohmann::json textureRole = {
                {"role", nativeTextureRoleName(nativeRole)},
                {"source", "mtlTextureMap"},
                {"mtlKey", mtlKey},
                {"material", materialName},
                {"confidence", "authored-slot"},
            };
            nlohmann::json texturePayload = nativeCookRuntimePayloadJson(
                textureCook,
                NativeAssetKind::Texture,
                textureGuid,
                workspace.root,
                effectiveSourcePath,
                sourceHash,
                importSettingsHash);
            texturePayload["assetIndex"] = textureIndex;
            texturePayload["sourceTexturePath"] = textureSourcePath.generic_string();
            texturePayload["textureRole"] = textureRole;
            texturePayload["persistentCacheReused"] = texturePersistentCacheReused;
            cookedPayloads.push_back(texturePayload);
            rootDependencies.push_back(textureGuid);
            (void)writeJson(texturePath, {
                {"version", 1},
                {"kind", "ImportedMtlTexture"},
                {"guid", textureGuid},
                {"sourcePath", textureSourcePath.generic_string()},
                {"rootSourcePath", effectiveSourceString},
                {"originalRootSourcePath", originalSourceString},
                {"copiedRootSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"runtimePayload", texturePayload},
                {"width", textureAsset.width},
                {"height", textureAsset.height},
                {"channels", textureAsset.channels},
                {"colorSpace", textureColorSpaceLabel(textureAsset)},
                {"textureRole", textureRole},
                {"mtl", {{"material", materialName}, {"key", mtlKey}, {"token", textureToken}}},
            });

            AssetRecord textureRecord;
            textureRecord.guid = textureGuid;
            textureRecord.type = AssetType::Texture;
            textureRecord.displayName = textureName;
            textureRecord.sourcePath = textureSourcePath.generic_string();
            textureRecord.importedPath = genericRelativeOrValue(texturePath, workspace.root);
            textureRecord.cachePath = texturePayload.value("cachePath", std::string{});
            textureRecord.sourceHash = sourceHash;
            textureRecord.importSettingsHash = importSettingsHash;
            textureRecord.lastModifiedTimestamp = timestampString();
            textureRecord.importSettings = request.settings;
            textureRecord.status = AssetImportStatus::Imported;
            records.push_back(std::move(textureRecord));

            generatedTextures.push_back({
                {"guid", textureGuid},
                {"name", textureName},
                {"sourcePath", textureSourcePath.generic_string()},
                {"path", genericRelativeOrValue(texturePath, workspace.root)},
                {"role", nativeTextureRoleName(nativeRole)},
                {"mtlKey", mtlKey},
            });
            return TextureAssetHandle{static_cast<uint32_t>(textureIndex)};
        };

        auto assignMtlTexture = [&](MaterialAsset& material, const nlohmann::json& textureMaps, const nlohmann::json& textureMapOptions, const char* mtlKey, nlohmann::json& textureDependencies, nlohmann::json& unboundTextures) {
            const nlohmann::json* mapValue = jsonObjectValueCaseInsensitive(textureMaps, mtlKey);
            if (mapValue == nullptr || !mapValue->is_string()) {
                return;
            }
            const std::string textureToken = mtlTexturePathValueToken(mapValue->get<std::string>());
            const std::string role = mtlTextureRoleStringForKey(mtlKey);
            const nlohmann::json options = mtlTextureMapOptionsForKey(textureMapOptions, mtlKey);
            const nlohmann::json optionDiagnostics = mtlTextureMapOptionRuntimeDiagnostics(options, mtlKey);
            std::optional<TextureAssetHandle> handle = cookMtlTexture(textureToken, role, material.name, mtlKey);
            if (!handle.has_value()) {
                return;
            }
            const size_t textureIndex = handle->index;
            const AssetGuid textureGuid = textureIndex < textureGuids.size() ? textureGuids[textureIndex] : std::string{};
            const std::string normalizedKey = lowerString(mtlKey);
            bool bound = true;
            if (normalizedKey == "map_kd") {
                material.baseColorTexture = *handle;
            } else if (normalizedKey == "map_ke") {
                material.emissiveTexture = *handle;
            } else if (normalizedKey == "map_bump" || normalizedKey == "bump") {
                material.normalTexture = *handle;
            } else if (normalizedKey == "map_ks") {
                material.hasSpecular = 1u;
                material.specularColorTexture = *handle;
            } else if (normalizedKey == "map_pr" || normalizedKey == "map_pm" || normalizedKey == "map_ns") {
                material.metallicRoughnessTexture = *handle;
            } else if (normalizedKey == std::string({'m', 'a', 'p', '_', 'd'})) {
                material.opacityTexture = *handle;
            } else if (normalizedKey == std::string({'d', 'i', 's', 'p'})) {
                material.heightTexture = *handle;
                material.heightScale = std::clamp(mtlTextureOptionFloat(mapValue->get<std::string>(), "-bm", material.heightScale), 0.0f, 0.25f);
            } else {
                bound = false;
            }
            if (bound) {
                textureDependencies.push_back({
                    {"guid", textureGuid},
                    {"role", role},
                    {"mtlKey", mtlKey},
                    {"textureMapOptions", options},
                    {"textureMapOptionDiagnostics", optionDiagnostics},
                });
            } else {
                unboundTextures.push_back({
                    {"guid", textureGuid},
                    {"role", role},
                    {"mtlKey", mtlKey},
                    {"textureMapOptions", options},
                    {"textureMapOptionDiagnostics", optionDiagnostics},
                    {"reason", "native-material-slot-not-yet-defined"},
                });
            }
        };

        const nlohmann::json emptyMtlMaterials = nlohmann::json::array();
        const nlohmann::json& mtlMaterials = mtlMetadata.contains("materials") && mtlMetadata["materials"].is_array()
            ? mtlMetadata["materials"]
            : emptyMtlMaterials;
        std::vector<AssetGuid> materialGuids;
        for (size_t i = 0; i < mtlMaterials.size(); ++i) {
            const nlohmann::json& materialJson = mtlMaterials[i];
            MaterialAsset material = mtlMaterialAssetFromMetadata(materialJson, "MtlMaterial_" + std::to_string(i));
            const std::string materialName = safeStem(material.name.empty() ? ("MtlMaterial_" + std::to_string(i)) : material.name);
            material.name = materialName;
            nlohmann::json textureDependencies = nlohmann::json::array();
            nlohmann::json unboundTextures = nlohmann::json::array();
            const nlohmann::json emptyTextureMaps = nlohmann::json::object();
            const nlohmann::json& textureMaps = materialJson.contains("textureMaps") ? materialJson["textureMaps"] : emptyTextureMaps;
            const nlohmann::json& textureMapOptions = materialJson.contains("textureMapOptions") ? materialJson["textureMapOptions"] : emptyTextureMaps;
            for (const char* key : {"map_Kd", "map_Ke", "map_Ks", "map_Ns", "map_Pr", "map_Pm", "map_bump", "bump", "map_d", "disp"}) {
                assignMtlTexture(material, textureMaps, textureMapOptions, key, textureDependencies, unboundTextures);
            }

            if (!request.settings.importMaterials) {
                continue;
            }
            const AssetGuid candidateMaterialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "MtlMaterial", materialGuids.size());
            const std::string materialContentKey = materialContentDedupKey(material, textureGuids);
            const auto existingMaterial = mtlCookedMaterialsByContent.find(materialContentKey);
            if (existingMaterial != mtlCookedMaterialsByContent.end()) {
                materialGuids.push_back(existingMaterial->second.guid);
                ++mtlMaterialContentReuseCount;
                mtlMaterialDedupAliases.push_back({
                    {"sourceIndex", i},
                    {"sourceName", materialName},
                    {"reusedSourceIndex", existingMaterial->second.sourceIndex},
                    {"reusedName", existingMaterial->second.name},
                    {"guid", existingMaterial->second.guid},
                    {"nativePath", genericRelativeOrValue(existingMaterial->second.nativePath, workspace.root)},
                });
                continue;
            }
            const AssetGuid materialGuid = candidateMaterialGuid;
            materialGuids.push_back(materialGuid);
            rootDependencies.push_back(materialGuid);
            const std::filesystem::path materialPath = importedDir / "Materials" / (materialName + ".rtmaterial.json");
            const std::filesystem::path nativeMaterialPath = cacheDir / "Materials" / (materialName + ".rtmaterial");
            std::filesystem::create_directories(materialPath.parent_path(), ec);
            std::filesystem::create_directories(nativeMaterialPath.parent_path(), ec);
            const std::optional<NativeAssetCookResult> reusableMaterial =
                reusableNativeCookResult(nativeMaterialPath, NativeAssetKind::Material, materialGuid, effectiveSourcePath);
            const bool materialPersistentCacheReused = reusableMaterial.has_value();
            if (materialPersistentCacheReused) {
                ++mtlMaterialPersistentCacheHitCount;
            }
            const NativeAssetCookResult materialCook = reusableMaterial.has_value()
                ? *reusableMaterial
                : nativeCooker.cookMaterial(
                    nativeCookInput(materialGuid, nativeMaterialPath, materialName),
                    material,
                    textureGuids);
            if (!recordNativeCookResult(materialCook, materialName)) {
                result.workerTotalMs = elapsedMilliseconds(workerStart);
                return result;
            }
            nlohmann::json materialPayload = nativeCookRuntimePayloadJson(
                materialCook,
                NativeAssetKind::Material,
                materialGuid,
                workspace.root,
                effectiveSourcePath,
                sourceHash,
                importSettingsHash);
            materialPayload["assetIndex"] = i;
            materialPayload["textureDependencyCount"] = textureDependencies.size();
            materialPayload["persistentCacheReused"] = materialPersistentCacheReused;
            materialPayload["contentDedupKey"] = materialContentKey;
            mtlCookedMaterialsByContent.emplace(materialContentKey, CookedAssetReuseEntry{
                .guid = materialGuid,
                .name = materialName,
                .importedPath = materialPath,
                .nativePath = nativeMaterialPath,
                .sourceIndex = i,
            });
            cookedPayloads.push_back(materialPayload);
            nlohmann::json pbrMetadata = materialPbrMetadataJson(material);
            pbrMetadata["workflow"] = "OBJ/MTL Phong-to-PBR";
            (void)writeJson(materialPath, {
                {"version", 1},
                {"kind", "ImportedMtlMaterial"},
                {"guid", materialGuid},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"runtimePayload", materialPayload},
                {"alphaMode", materialAlphaModeLabel(material.alphaMode)},
                {"pbr", pbrMetadata},
                {"mtl", materialJson},
                {"conversion", {
                    {"schema", "MtlPhongToPbrConversionV1"},
                    {"lossy", true},
                    {"rules", nlohmann::json::array({"Kd->baseColor", "Ke->emissive", "Ks->specularColor", "Ns->roughness", "d/Tr->alpha", "Pm->metallic", "Pr->roughness"})},
                }},
                {"textureDependencies", textureDependencies},
                {"unboundTextureMaps", unboundTextures},
            });

            AssetRecord materialRecord;
            materialRecord.guid = materialGuid;
            materialRecord.type = AssetType::Material;
            materialRecord.displayName = materialName;
            materialRecord.sourcePath = effectiveSourceString;
            materialRecord.importedPath = genericRelativeOrValue(materialPath, workspace.root);
            materialRecord.cachePath = materialPayload.value("cachePath", std::string{});
            for (const auto& dep : textureDependencies) {
                materialRecord.dependencies.push_back(AssetDependency{dep.value("guid", std::string{}), dep.value("role", std::string{})});
            }
            materialRecord.sourceHash = sourceHash;
            materialRecord.importSettingsHash = importSettingsHash;
            materialRecord.lastModifiedTimestamp = timestampString();
            materialRecord.importSettings = request.settings;
            materialRecord.status = AssetImportStatus::Imported;
            records.push_back(std::move(materialRecord));

            generatedMaterials.push_back({
                {"guid", materialGuid},
                {"name", materialName},
                {"path", genericRelativeOrValue(materialPath, workspace.root)},
                {"textureDependencyCount", textureDependencies.size()},
                {"unboundTextureMapCount", unboundTextures.size()},
            });
        }

        runtimePayload = {
            {"kind", "MtlRuntimeMaterialLibraryPayload"},
            {"sourcePath", effectiveSourceString},
            {"originalSourcePath", originalSourceString},
            {"copiedSourcePath", copiedSourceString},
            {"sourceHash", sourceHash},
            {"importSettingsHash", importSettingsHash},
            {"available", std::filesystem::exists(effectiveSourcePath)},
            {"validForSource", true},
            {"runtimeMaterialCooked", !materialGuids.empty()},
            {"runtimeTexturesCooked", !textureGuids.empty()},
            {"materialCount", materialGuids.size()},
            {"textureCount", textureGuids.size()},
            {"texturePersistentCacheHitCount", mtlTexturePersistentCacheHitCount},
            {"materialDeduplication", {
                {"sourceMaterialCount", request.settings.importMaterials ? mtlMaterials.size() : 0u},
                {"uniqueMaterialCount", mtlCookedMaterialsByContent.size()},
                {"reusedMaterialCount", mtlMaterialContentReuseCount},
                {"persistentCacheHitCount", mtlMaterialPersistentCacheHitCount},
                {"aliases", mtlMaterialDedupAliases},
            }},
            {"materials", generatedMaterials},
            {"textures", generatedTextures},
            {"metadata", mtlMetadata},
        };
        cookedPayloads.push_back(runtimePayload);
        placeholder["runtimePayload"] = runtimePayload;
        placeholder["thumbnail"] = thumbnailMetadata;
        placeholder["mtlMetadata"] = mtlMetadata;
        placeholder["materialAssets"] = generatedMaterials;
        placeholder["textureAssets"] = generatedTextures;
        placeholder["runtimeMaterialCooked"] = !materialGuids.empty();
        placeholder["runtimeTexturesCooked"] = !textureGuids.empty();
        placeholder["texturePersistentCacheHitCount"] = mtlTexturePersistentCacheHitCount;
        placeholder["materialDeduplication"] = runtimePayload["materialDeduplication"];
        placeholder["sourceExtension"] = sourceExtension;
        placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
        placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
        cache["runtimePayload"] = runtimePayload;
        cache["cookedPayloads"] = cookedPayloads;
        cache["thumbnail"] = thumbnailMetadata;
        cache["mtlMetadata"] = mtlMetadata;
        cache["materialAssets"] = generatedMaterials;
        cache["textureAssets"] = generatedTextures;
        cache["texturePersistentCacheHitCount"] = mtlTexturePersistentCacheHitCount;
        cache["materialDeduplication"] = runtimePayload["materialDeduplication"];
    } else if (sourceIsStandaloneTexture) {
        const auto inspectStart = std::chrono::steady_clock::now();
        setProgress(0.45f, type == AssetType::HDRI ? "Inspecting HDRI source" : "Inspecting texture source");
        textureRoleMetadata = inferTextureRole(effectiveSourcePath, type);
        const CompressedTextureKind compressedKind = detectCompressedTextureKind(effectiveSourcePath.string());
        const bool sourceIsStandaloneBasis = compressedKind == CompressedTextureKind::BasisStandalone;
        const bool sourceIsKtx2 = compressedKind == CompressedTextureKind::Ktx2;
        standaloneBasisUnsupported = sourceIsStandaloneBasis;
        nlohmann::json ktx2CookPolicy = nlohmann::json::object();
        if (sourceIsKtx2) {
            const Ktx2ContainerInfo ktx2Info = inspectKtx2Container(effectiveSourcePath.string());
            ktx2CookPolicy = {
                {"container", "ktx2"},
                {"valid", ktx2Info.valid},
                {"vkFormat", ktx2Info.vkFormat},
                {"width", ktx2Info.width},
                {"height", ktx2Info.height},
                {"levelCount", ktx2Info.levelCount},
                {"supercompressionScheme", ktx2Info.supercompressionScheme},
                {"supercompressionName", ktx2Info.supercompressionName},
                {"basisUniversalSupercompressed", ktx2Info.basisUniversalSupercompressed},
                {"preserveNativePayload", ktx2Info.preserveNativePayload},
                {"requiresTranscode", ktx2Info.requiresTranscode},
                {"policy", ktx2Info.policy},
            };
        }
        if (sourceIsStandaloneBasis) {
            result.warnings.push_back("Standalone .basis texture import is unsupported; wrap BasisU payloads in KTX2/KHR_texture_basisu before import.");
        }

        std::filesystem::path payloadPath;
        if (!sourceIsStandaloneBasis) {
            payloadPath = cacheDir / (name + effectiveSourcePath.extension().string());
            std::filesystem::copy_file(effectiveSourcePath, payloadPath, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                result.errors.push_back("Could not copy texture payload to cache: " + ec.message());
                result.workerInspectMs = elapsedMilliseconds(inspectStart);
                result.workerTotalMs = elapsedMilliseconds(workerStart);
                return result;
            }
            result.generatedFiles.push_back(payloadPath);
            rootThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(payloadPath, workspace.root) : std::string{};
        }

        bool inspected = false;
        TextureData textureData;
        if (!sourceIsStandaloneBasis) {
            try {
                const NativeTextureRole nativeTextureRole = nativeTextureRoleFromString(textureRoleMetadata.value("role", std::string("unknown")));
                const NativeTextureColorSpace nativeTextureColorSpace = type == AssetType::HDRI
                    ? NativeTextureColorSpace::HdrLinear
                    : ((nativeTextureRole == NativeTextureRole::BaseColor || nativeTextureRole == NativeTextureRole::Emissive)
                        ? NativeTextureColorSpace::Srgb
                        : NativeTextureColorSpace::Linear);
                textureData = TextureLoader::load(
                    effectiveSourcePath.string(),
                    importTextureFormatSupport,
                    nativeTextureRole,
                    nativeTextureColorSpace);
                inspected = true;
            } catch (const std::exception& ex) {
                result.warnings.push_back("Texture dimension/format inspection failed; metadata records file payload only: " + std::string(ex.what()));
            }
        }
        result.workerInspectMs = elapsedMilliseconds(inspectStart);

        std::optional<NativeAssetCookResult> nativeTextureCook;
        if (inspected) {
            TextureAsset textureAsset;
            textureAsset.name = name;
            textureAsset.sourcePath = effectiveSourcePath;
            textureAsset.width = static_cast<uint32_t>(std::max(0, textureData.width));
            textureAsset.height = static_cast<uint32_t>(std::max(0, textureData.height));
            textureAsset.channels = 4;
            textureAsset.sourceArrayLayers = textureData.sourceArrayLayers;
            textureAsset.sourceDepth = textureData.sourceDepth;
            textureAsset.sourceFaceCount = textureData.sourceFaceCount;
            textureAsset.sourceIsCubemap = textureData.sourceIsCubemap;
            textureAsset.mipLevels = std::max(1, textureData.mipLevels);
            textureAsset.srgb = !(textureData.linearColorSpace || type == AssetType::HDRI);
            textureAsset.isCompressed = textureData.isCompressed;
            textureAsset.linearColorSpace = textureData.linearColorSpace || type == AssetType::HDRI;
            textureAsset.format = textureData.format;
            textureAsset.compressedFormat = textureData.compressedFormat;
            textureAsset.sourceContainerKind = textureData.sourceContainerKind;
            textureAsset.nativePayloadSource = textureData.nativePayloadSource;
            textureAsset.sourceContainerPreserved = textureData.sourceContainerPreserved;
            textureAsset.sourceContainerTranscoded = textureData.sourceContainerTranscoded;
            textureAsset.rgba8 = textureData.pixels;
            textureAsset.mipData = textureData.mipData;
            const std::filesystem::path nativeTexturePath = cacheDir / (name + ".rttexture");
            NativeAssetCookResult cook = nativeCooker.cookTexture(
                nativeCookInput(guid, nativeTexturePath, name),
                textureAsset,
                textureRoleMetadata.value("role", std::string("unknown")));
            if (!recordNativeCookResult(cook, name)) {
                result.workerTotalMs = elapsedMilliseconds(workerStart);
                return result;
            }
            nativeTextureCook = std::move(cook);
        }

        if (sourceIsStandaloneBasis) {
            runtimePayload = {
                {"kind", "UnsupportedStandaloneBasisPayload"},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"available", false},
                {"validForSource", false},
                {"textureRole", textureRoleMetadata},
                {"unsupportedReason", "Standalone .basis files are unsupported; wrap BasisU payloads in KTX2/KHR_texture_basisu."},
            };
        } else {
            runtimePayload = {
                {"kind", type == AssetType::HDRI ? "LooseHDRIPayload" : "LooseTexturePayload"},
                {"cachePath", genericRelativeOrValue(payloadPath, workspace.root)},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"payloadHash", fileFingerprintString(payloadPath)},
                {"payloadBytes", fileSizeOrZero(payloadPath)},
                {"available", std::filesystem::exists(payloadPath)},
                {"validForSource", true},
                {"textureRole", textureRoleMetadata},
            };
        }
        if (!ktx2CookPolicy.empty()) {
            runtimePayload["ktx2CookPolicy"] = ktx2CookPolicy;
        }
        if (nativeTextureCook.has_value()) {
            runtimePayload = nativeCookRuntimePayloadJson(
                *nativeTextureCook,
                NativeAssetKind::Texture,
                guid,
                workspace.root,
                effectiveSourcePath,
                sourceHash,
                importSettingsHash);
            runtimePayload["textureRole"] = textureRoleMetadata;
            runtimePayload["looseSourcePayload"] = genericRelativeOrValue(payloadPath, workspace.root);
            if (!ktx2CookPolicy.empty()) {
                runtimePayload["ktx2CookPolicy"] = ktx2CookPolicy;
            }
        }
        thumbnailMetadata = thumbnailMetadataJson(
            type == AssetType::HDRI ? "LooseHDRIPayloadPreview" : "LooseTexturePayloadPreview",
            rootThumbnailPath,
            sourceHash,
            importSettingsHash,
            runtimePayload.value("payloadHash", std::string{}));
        cookedPayloads.push_back(runtimePayload);

        placeholder["runtimePayload"] = runtimePayload;
        placeholder["thumbnail"] = thumbnailMetadata;
        placeholder["textureRole"] = textureRoleMetadata;
        placeholder["sourceExtension"] = lowerString(effectiveSourcePath.extension().string());
        placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
        placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
        placeholder["inspected"] = inspected;
        placeholder["compressedTextureKind"] = std::string(compressedTextureKindName(compressedKind));
        placeholder["basisStandaloneUnsupported"] = sourceIsStandaloneBasis;
        if (!ktx2CookPolicy.empty()) {
            placeholder["ktx2CookPolicy"] = ktx2CookPolicy;
        }
        if (inspected) {
            placeholder["width"] = textureData.width;
            placeholder["height"] = textureData.height;
            placeholder["channels"] = 4;
            placeholder["mipLevels"] = std::max(1, textureData.mipLevels);
            placeholder["format"] = textureFormatLabel(textureData.format);
            placeholder["compressedFormat"] = textureFormatLabel(textureData.compressedFormat);
            placeholder["isCompressed"] = textureData.isCompressed;
            placeholder["colorSpace"] = textureData.linearColorSpace || type == AssetType::HDRI ? "Linear" : "sRGB";
            placeholder["payloadLayout"] = textureData.mipData.empty() ? "singleImage" : "mipChain";
        }

        cache["runtimePayload"] = runtimePayload;
        cache["cookedPayloads"] = cookedPayloads;
        cache["thumbnail"] = thumbnailMetadata;
        cache["textureRole"] = textureRoleMetadata;
        cache["sourceExtension"] = placeholder["sourceExtension"];
        cache["inspected"] = inspected;
        cache["compressedTextureKind"] = placeholder["compressedTextureKind"];
        cache["basisStandaloneUnsupported"] = sourceIsStandaloneBasis;
        if (!ktx2CookPolicy.empty()) {
            cache["ktx2CookPolicy"] = ktx2CookPolicy;
        }
        if (inspected) {
            cache["width"] = textureData.width;
            cache["height"] = textureData.height;
            cache["format"] = textureFormatLabel(textureData.format);
            cache["isCompressed"] = textureData.isCompressed;
        }
    } else {
        setProgress(0.55f, "Preparing placeholder metadata");
    }
    if (!result.errors.empty()) {
        result.workerTotalMs = elapsedMilliseconds(workerStart);
        return result;
    }
    importerCapabilities = importerCapabilityReport(
        effectiveSourcePath,
        type,
        request.settings,
        objMetadata,
        mtlMetadata,
        usdMetadata,
        skeletalAnimationMetadata,
        collisionLodMetadata,
        result.warnings);
    placeholder["importerCapabilities"] = importerCapabilities;
    cache["importerCapabilities"] = importerCapabilities;
    const auto writeStart = std::chrono::steady_clock::now();
    setProgress(0.92f, "Writing import report");
    placeholder["dependencies"] = rootDependencies;
    placeholder["importGroup"] = {
        {"id", sourceHash + ":" + importSettingsHash},
        {"name", name},
        {"rootGuid", guid},
    };
    cache["importGroup"] = placeholder["importGroup"];
    nlohmann::json dependencyRecords = nlohmann::json::array();
    for (const AssetGuid& dependency : rootDependencies) {
        dependencyRecords.push_back({{"guid", dependency}, {"kind", "importedChild"}});
    }
    placeholder["dependencyRecords"] = dependencyRecords;
    std::vector<std::string> generatedFilePaths;
    generatedFilePaths.reserve(result.generatedFiles.size() + 2);
    for (const std::filesystem::path& path : result.generatedFiles) {
        generatedFilePaths.push_back(path.generic_string());
    }
    generatedFilePaths.push_back(importedPath.generic_string());
    generatedFilePaths.push_back(cachePath.generic_string());
    std::sort(generatedFilePaths.begin(), generatedFilePaths.end());
    generatedFilePaths.erase(std::unique(generatedFilePaths.begin(), generatedFilePaths.end()), generatedFilePaths.end());
    nlohmann::json generatedFilesJson = nlohmann::json::array();
    for (const std::string& path : generatedFilePaths) {
        generatedFilesJson.push_back(path);
    }
    nlohmann::json report = {
        {"version", 1},
        {"kind", "ImportReport"},
        {"guid", guid},
        {"sourcePath", effectiveSourceString},
        {"sourceHash", sourceHash},
        {"importSettingsHash", importSettingsHash},
        {"sourceControlPolicy", sourceControlPolicy},
        {"importProvenance", {
            {"originalSourcePath", originalSourceString},
            {"effectiveSourcePath", effectiveSourceString},
            {"copiedSourcePath", copiedSourceString},
            {"sourceReferenceMode", copiedSourcePath.empty() ? "ExternalReference" : "CopiedIntoProject"},
            {"importer", sourceIsFbx ? "FbxAssimpStaticImporter" : sourceIsUsd ? "UsdOpenUsdStageMetadataImporter" : sourceIsObj ? "ObjMetadataImporter" : sourceIsMtl ? "MtlMetadataImporter" : importerLabelForType(type)},
            {"importerVersion", 1},
            {"mode", request.mode},
            {"settings", {
                {"copySourceIntoProject", request.settings.copySourceIntoProject},
                {"preserveHierarchy", request.settings.preserveHierarchy},
                {"importMaterials", request.settings.importMaterials},
                {"importTextures", request.settings.importTextures},
                {"importCameras", request.settings.importCameras},
                {"importLights", request.settings.importLights},
                {"generateTangents", request.settings.generateTangents},
                {"buildBlasCache", request.settings.buildBlasCache},
                {"generatePrefabAsset", request.settings.generatePrefabAsset},
                {"buildCookedPayloadsNow", request.settings.buildCookedPayloadsNow},
                {"generateThumbnails", request.settings.generateThumbnails},
                {"unitScale", request.settings.unitScale},
                {"coordinateConversion", request.settings.coordinateConversion},
                {"materialImportMode", request.settings.materialImportMode},
                {"textureImportMode", request.settings.textureImportMode},
                {"textureCompression", request.settings.textureCompression},
                {"emissiveScale", request.settings.emissiveScale},
            }},
        }},
        {"runtimePayload", runtimePayload.is_null() ? nlohmann::json::object() : runtimePayload},
        {"cookedPayloads", cookedPayloads},
        {"thumbnail", thumbnailMetadata.is_null() ? nlohmann::json::object() : thumbnailMetadata},
        {"importerCapabilities", importerCapabilities.is_null() ? nlohmann::json::object() : importerCapabilities},
        {"textureRole", textureRoleMetadata.is_null() ? nlohmann::json::object() : textureRoleMetadata},
        {"objMetadata", objMetadata.is_null() ? nlohmann::json::object() : objMetadata},
        {"mtlMetadata", mtlMetadata.is_null() ? nlohmann::json::object() : mtlMetadata},
        {"fbxMetadata", fbxMetadata.is_null() ? nlohmann::json::object() : fbxMetadata},
        {"usdMetadata", usdMetadata.is_null() ? nlohmann::json::object() : usdMetadata},
        {"skeletalAnimationMetadata", skeletalAnimationMetadata.is_null() ? nlohmann::json::object() : skeletalAnimationMetadata},
        {"collisionLodMetadata", collisionLodMetadata.is_null() ? nlohmann::json::object() : collisionLodMetadata},
        {"dependencyRecords", dependencyRecords},
        {"generatedFiles", generatedFilesJson},
        {"warnings", result.warnings},
        {"errors", result.errors},
        {"timings_ms", {
            {"total", result.workerTotalMs},
            {"validate", result.workerValidateMs},
            {"directories", result.workerDirectoryMs},
            {"inspect", result.workerInspectMs},
            {"write", result.workerWriteMs},
        }},
        {"sceneMutation", false},
        {"rendererResourcesCreated", false},
    };

    if (!writeJson(importedPath, placeholder) || !writeJson(cachePath, cache) || !writeJson(reportPath, report)) {
        result.workerWriteMs = elapsedMilliseconds(writeStart);
        result.workerTotalMs = elapsedMilliseconds(workerStart);
        return result;
    }
    result.workerWriteMs = elapsedMilliseconds(writeStart);
    result.workerTotalMs = elapsedMilliseconds(workerStart);
    report["timings_ms"] = {
        {"total", result.workerTotalMs},
        {"validate", result.workerValidateMs},
        {"directories", result.workerDirectoryMs},
        {"inspect", result.workerInspectMs},
        {"write", result.workerWriteMs},
    };
    if (std::ofstream reportFile(reportPath); reportFile.is_open()) {
        reportFile << report.dump(2);
    }

    AssetRecord record;
    record.guid = guid;
    record.type = type;
    record.displayName = name;
    record.sourcePath = effectiveSourceString;
    record.importedPath = genericRelativeOrValue(importedPath, workspace.root);
    record.cachePath = runtimePayload.is_object()
        ? runtimePayload.value("cachePath", genericRelativeOrValue(cachePath, workspace.root))
        : genericRelativeOrValue(cachePath, workspace.root);
    record.thumbnailPath = rootThumbnailPath;
    record.sourceHash = sourceHash;
    record.importSettingsHash = importSettingsHash;
    record.lastModifiedTimestamp = timestampString();
    record.importSettings = request.settings;
    record.status = (standaloneBasisUnsupported || importFailed) ? AssetImportStatus::Failed : AssetImportStatus::Imported;
    for (const AssetGuid& dependency : rootDependencies) {
        record.dependencies.push_back(AssetDependency{dependency, "source"});
    }
    const std::string importGroupId = sourceHash + ":" + importSettingsHash;
    const std::string importGroupName = name;
    auto stampImportGroup = [&](AssetRecord& assetRecord) {
        assetRecord.importGroupId = importGroupId;
        assetRecord.importGroupName = importGroupName;
        assetRecord.importRootGuid = guid;
    };
    stampImportGroup(record);
    for (AssetRecord& childRecord : records) {
        stampImportGroup(childRecord);
    }
    result.record = std::move(record);
    result.records = std::move(records);
    result.records.insert(result.records.begin(), result.record);
    result.importReportPath = reportPath;
    result.success = !(standaloneBasisUnsupported || importFailed);
    setProgress(1.0f, "Import staged");
    return result;
}

} // namespace rtv
