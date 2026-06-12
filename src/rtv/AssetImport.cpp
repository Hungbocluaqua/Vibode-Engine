#include "rtv/AssetImport.h"

#include "rtv/AnimationController.h"
#include "rtv/AssetManager.h"
#include "rtv/GltfLoader.h"
#include "rtv/NativeAssetCooker.h"
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <glm/gtc/quaternion.hpp>

#if RTV_ENABLE_TINYOBJ_IMPORTER && RTV_TINYOBJ_IMPORTER_AVAILABLE
#define TINYOBJLOADER_IMPLEMENTATION
#define TINYOBJLOADER_DISABLE_FAST_FLOAT
#include <tiny_obj_loader.h>
#endif

#if RTV_ENABLE_ASSIMP_IMPORTER && RTV_ASSIMP_IMPORTER_AVAILABLE
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

#if RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
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

uintmax_t fileSizeOrZero(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0u : size;
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
    case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3_UNORM";
    case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3_SRGB";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5_UNORM";
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

nlohmann::json materialPbrMetadataJson(const MaterialAsset& material) {
    return {
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
        {"metallicRoughness", "Linear", {"metallicroughness", "metalrough", "metal_rough", "orm", "rma", "mrao"}},
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
                {"runtimeSupport", "metadata_only_collision_cook_pending"},
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
                {"runtimeSupport", "metadata_only_lod_cook_pending"},
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
        {"runtimeSupport", "metadata_only_collision_lod_cook_pending"},
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
            continue;
        }
        if (currentMaterial.is_null() || !currentMaterial.is_object()) {
            currentMaterial = {
                {"name", "DefaultMaterial"},
                {"properties", nlohmann::json::object()},
                {"textureMaps", nlohmann::json::object()},
                {"textureMapRoles", nlohmann::json::object()},
            };
        }
        if (isMtlTextureMapKey(key)) {
            const std::string texturePath = mtlTexturePathToken(tokens);
            currentMaterial["textureMaps"][key] = trimAscii(std::string_view(trimmed).substr(key.size()));
            currentMaterial["textureMapRoles"][key] = mtlTextureMapRoleLabel(key);
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
    float alpha = mtlScalarProperty(properties, "d", 1.0f);
    if (jsonObjectValueCaseInsensitive(properties, "Tr") != nullptr) {
        alpha = 1.0f - mtlScalarProperty(properties, "Tr", 0.0f);
    }
    material.baseColorFactor.w = std::clamp(alpha, 0.0f, 1.0f);
    if (material.baseColorFactor.w < 0.999f) {
        material.alphaMode = kMaterialAlphaModeBlend;
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
        addWarningOnce(warnings, "Source contains collision-named OBJ objects/groups; collision metadata was preserved, but runtime collision cooking is not implemented yet.");
    }
    if (!lodCandidates.empty()) {
        addWarningOnce(warnings, "Source contains LOD-named OBJ objects/groups; LOD metadata was preserved, but runtime LOD cooking/selection is not implemented yet.");
    }

    return {
        {"inspected", true},
        {"format", "OBJ"},
        {"vertexCount", vertexCount},
        {"texcoordCount", texcoordCount},
        {"normalCount", normalCount},
        {"faceCount", faceCount},
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
    size_t skippedFaces = 0;
    bool generatedAnyNormals = false;

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
            for (size_t vertexInFace = 0; vertexInFace < 3; ++vertexInFace) {
                const tinyobj::index_t index = shape.mesh.indices[indexOffset + vertexInFace];
                MeshVertex vertex;
                if (index.vertex_index >= 0) {
                    const size_t source = static_cast<size_t>(index.vertex_index) * 3u;
                    if (source + 2u < attrib.vertices.size()) {
                        vertex.position = glm::vec3{attrib.vertices[source], attrib.vertices[source + 1u], attrib.vertices[source + 2u]};
                    }
                }
                if (index.normal_index >= 0) {
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
                        triangleMissingNormal = true;
                    }
                } else {
                    triangleMissingNormal = true;
                }
                if (index.texcoord_index >= 0) {
                    const size_t source = static_cast<size_t>(index.texcoord_index) * 2u;
                    if (source + 1u < attrib.texcoords.size()) {
                        vertex.texcoord = glm::vec2{attrib.texcoords[source], 1.0f - attrib.texcoords[source + 1u]};
                    }
                }
                out.mesh.vertices.push_back(vertex);
                out.mesh.indices.push_back(static_cast<uint32_t>(out.mesh.vertices.size() - 1u));
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
            }
            indexOffset += faceVertexCount;
        }
        if (primitiveMaterialId != std::numeric_limits<int>::min()) {
            appendPrimitive(primitiveFirstVertex, primitiveFirstIndex, primitiveMaterialId, shapeLabel);
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
        {"shapeCount", shapes.size()},
        {"materialCount", materials.size()},
        {"usedMaterialCount", usedMaterialIds.size()},
        {"vertexCount", out.mesh.vertices.size()},
        {"indexCount", out.mesh.indices.size()},
        {"primitiveCount", out.mesh.primitives.size()},
        {"generatedNormals", generatedAnyNormals},
        {"skippedFaceCount", skippedFaces},
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
        {"runtimeSupport", "metadata_bridge_only_runtime_skinning_pending"},
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
    for (unsigned animationIndex = 0; animationIndex < imported->mNumAnimations; ++animationIndex) {
        const aiAnimation* animation = imported->mAnimations[animationIndex];
        if (animation == nullptr) {
            continue;
        }
        nlohmann::json channels = nlohmann::json::array();
        uint32_t decodedChannelCount = 0;
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
        animations.push_back({
            {"schema", "FbxAnimationMetadataV1"},
            {"index", animationIndex},
            {"name", animation->mName.length > 0 ? assimpString(animation->mName) : ("FbxAnimation_" + std::to_string(animationIndex))},
            {"durationTicks", animation->mDuration},
            {"ticksPerSecond", ticksPerSecond},
            {"durationSeconds", ticksPerSecond > 0.0 ? animation->mDuration / ticksPerSecond : 0.0},
            {"channelCount", animation->mNumChannels},
            {"decodedChannelCount", decodedChannelCount},
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
    const NativeTextureFormatSupport& textureFormatSupport) {
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
                out.warnings.push_back("FBX mesh '" + assimpString(meshSource->mName) + "' has bones; skeleton metadata is preserved, while runtime GPU skinning remains future work.");
            }
        }
    }
    out.skeletons = fbxSkeletonMetadataFromAssimp(imported);
    out.animations = fbxAnimationMetadataFromAssimp(imported);

    out.scene.name = displayName.empty() ? sourcePath.stem().string() : std::string(displayName);
    out.scene.sourcePath = sourcePath;
    const FbxTextureConvention textureConvention = detectFbxTextureConvention(sourcePath);

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
        for (unsigned faceIndex = 0; faceIndex < meshSource->mNumFaces; ++faceIndex) {
            const aiFace& face = meshSource->mFaces[faceIndex];
            if (face.mNumIndices != 3) {
                continue;
            }
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
        mesh.primitives.push_back(primitive);
        MeshAssetHandle handle = out.assets.addMesh(mesh);
        out.scene.meshes.push_back(handle);
        meshReports.push_back({
            {"index", meshIndex},
            {"name", mesh.name},
            {"vertexCount", mesh.vertices.size()},
            {"indexCount", mesh.indices.size()},
            {"materialIndex", meshSource->mMaterialIndex},
            {"hasVertexColors", meshSource->HasVertexColors(0)},
            {"vertexColorPolicy", ignoreVertexColors ? "ignored_fbx_blend_mask" : (meshSource->HasVertexColors(0) ? "imported_as_color" : "none")},
            {"skinning", skinningReport},
        });
    }

    std::unordered_map<const aiNode*, uint32_t> nodeIndices;
    std::function<void(const aiNode*, int32_t)> appendNode = [&](const aiNode* node, int32_t parent) {
        if (node == nullptr) {
            return;
        }
        const uint32_t nodeIndex = static_cast<uint32_t>(out.scene.nodes.size());
        nodeIndices[node] = nodeIndex;
        SceneNodeAsset sceneNode;
        sceneNode.name = node->mName.length > 0 ? assimpString(node->mName) : "FbxNode_" + std::to_string(nodeIndex);
        sceneNode.transform = assimpMatrixToGlm(node->mTransformation);
        sceneNode.parent = parent;
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
                meshNode.transform = glm::mat4(1.0f);
                meshNode.parent = static_cast<int32_t>(nodeIndex);
                meshNode.mesh = out.scene.meshes[meshIndex];
                if (!out.skeletons.empty() && imported->mMeshes[meshIndex] != nullptr && imported->mMeshes[meshIndex]->HasBones()) {
                    meshNode.skinIndex = 0;
                }
                out.scene.nodes.push_back(std::move(meshNode));
                out.scene.nodes[static_cast<size_t>(nodeIndex)].children.push_back(meshNodeIndex);
            }
        }
        for (unsigned childIndex = 0; childIndex < node->mNumChildren; ++childIndex) {
            appendNode(node->mChildren[childIndex], static_cast<int32_t>(nodeIndex));
        }
    };
    appendNode(imported->mRootNode, -1);

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
                node.cameraYfov = camera->mHorizontalFOV;
                node.cameraAspectRatio = camera->mAspect;
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
                sceneLight.transform = out.scene.nodes[nodeIndex].transform;
                break;
            }
        }
        sceneLight.color = glm::vec3{light->mColorDiffuse.r, light->mColorDiffuse.g, light->mColorDiffuse.b};
        sceneLight.intensity = 1.0f;
        sceneLight.type = light->mType == aiLightSource_DIRECTIONAL ? 0u : light->mType == aiLightSource_SPOT ? 2u : 1u;
        sceneLight.innerConeRadians = light->mAngleInnerCone;
        sceneLight.outerConeRadians = light->mAngleOuterCone;
        out.scene.lights.push_back(sceneLight);
    }

    if (out.scene.meshes.empty()) {
        out.errors.push_back("FBX source did not contain renderable static meshes.");
    }
    out.diagnostics = {
        {"schema", "FbxStaticImportDiagnosticsV1"},
        {"parser", "assimp"},
        {"supported", true},
        {"meshCount", out.scene.meshes.size()},
        {"materialCount", out.scene.materials.size()},
        {"textureCount", out.assets.textures().size()},
        {"nodeCount", out.scene.nodes.size()},
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
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    size_t meshCount = 0;
    size_t materialBindingCount = 0;
    size_t cameraCount = 0;
    size_t lightCount = 0;
};

struct UsdRuntimeMeshCookData {
    bool supported = false;
    std::vector<MeshAsset> meshes;
    std::vector<std::string> materialBindingPaths;
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
        out.warnings.push_back("USDZ packaged texture provenance was preserved; extraction, native .rttexture cook, and shader binding remain future work.");
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
    if (lower.find("normal") != std::string::npos) {
        return NativeTextureRole::Normal;
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
    if (lower.find("basecolor") != std::string::npos || lower.find("diffuse") != std::string::npos || lower.find("file") != std::string::npos) {
        return NativeTextureRole::BaseColor;
    }
    return NativeTextureRole::Unknown;
}

NativeTextureColorSpace colorSpaceForTextureRole(NativeTextureRole role) {
    return role == NativeTextureRole::BaseColor || role == NativeTextureRole::Emissive
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
    const std::string shaderId = reference.value("shaderId", std::string{});
    if (lowerString(shaderId).find("uvtexture") != std::string::npos) {
        return NativeTextureRole::BaseColor;
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
    case NativeTextureRole::Opacity: return "baseColor";
    case NativeTextureRole::Height: return "normal";
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
        material.baseColorTexture = handle;
        return true;
    case NativeTextureRole::Height:
        material.normalTexture = handle;
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
    glm::mat4 out{1.0f};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            out[col][row] = static_cast<float>(matrix[row][col]);
        }
    }
    return out;
}

glm::vec3 glmVec3FromUsd(const pxr::GfVec3f& value) {
    return glm::vec3{value[0], value[1], value[2]};
}

nlohmann::json usdPrimTransformJson(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache* xformCache = nullptr) {
    pxr::UsdGeomXformable xformable(prim);
    if (!xformable) {
        return nlohmann::json::object();
    }
    pxr::GfMatrix4d transform(1.0);
    bool resetsXformStack = false;
    const bool hasTransform = xformable.GetLocalTransformation(&transform, &resetsXformStack);
    nlohmann::json result = {
        {"hasTransform", hasTransform},
        {"resetsXformStack", resetsXformStack},
        {"placementTransform", transformJsonFromMatrix(glmMat4FromUsdMatrix(transform))},
        {"localMatrix", usdMatrixJson(transform)},
    };
    if (xformCache != nullptr) {
        const pxr::GfMatrix4d worldTransform = xformCache->GetLocalToWorldTransform(prim);
        result["parentHierarchyTransformComposed"] = true;
        result["worldPlacementTransform"] = transformJsonFromMatrix(glmMat4FromUsdMatrix(worldTransform));
        result["worldMatrix"] = usdMatrixJson(worldTransform);
    } else {
        result["parentHierarchyTransformComposed"] = false;
    }
    return result;
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

nlohmann::json usdMaterialTextureReferencesJson(const pxr::UsdPrim& root) {
    nlohmann::json references = nlohmann::json::array();
    std::unordered_set<std::string> seen;
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
            const std::string key = shaderPath + "|" + attributeName + "|" + authoredPath + "|" + resolvedPath;
            if (!seen.insert(key).second) {
                continue;
            }
            pxr::TfToken shaderId;
            (void)usdReadAttribute(prim, "info:id", shaderId);
            references.push_back({
                {"shaderPath", shaderPath},
                {"shaderTypeName", prim.GetTypeName().GetString()},
                {"shaderId", shaderId.GetString()},
                {"attribute", attributeName},
                {"assetPath", authoredPath},
                {"resolvedPath", resolvedPath},
                {"nativeTextureCookImplemented", false},
            });
        }
    }
    return references;
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
    textureReferences = usdMaterialTextureReferencesJson(materialPrim);
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
    return {
        {"hasPoints", mesh.GetPointsAttr().HasAuthoredValueOpinion() || mesh.GetPointsAttr().HasValue()},
        {"hasFaceVertexCounts", mesh.GetFaceVertexCountsAttr().HasAuthoredValueOpinion() || mesh.GetFaceVertexCountsAttr().HasValue()},
        {"hasFaceVertexIndices", mesh.GetFaceVertexIndicesAttr().HasAuthoredValueOpinion() || mesh.GetFaceVertexIndicesAttr().HasValue()},
        {"hasNormals", mesh.GetNormalsAttr().HasAuthoredValueOpinion() || mesh.GetNormalsAttr().HasValue()},
        {"hasUvPrimvar", prim.HasAttribute(pxr::TfToken("primvars:st")) || prim.HasAttribute(pxr::TfToken("primvars:UVMap"))},
        {"hasVertexColorPrimvar", prim.HasAttribute(pxr::TfToken("primvars:displayColor")) || prim.HasAttribute(pxr::TfToken("displayColor"))},
        {"materialBound", usdPrimHasAuthoredMaterialBinding(prim)},
    };
}

glm::vec3 normalizedOrFallback(glm::vec3 value, glm::vec3 fallback) {
    const float length2 = glm::dot(value, value);
    return length2 > 1.0e-12f ? value / std::sqrt(length2) : fallback;
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

nlohmann::json usdCameraRuntimeJson(const pxr::UsdGeomCamera& camera, pxr::UsdGeomXformCache* xformCache = nullptr) {
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
    return {
        {"primPath", prim.GetPath().GetString()},
        {"name", prim.GetName().GetString()},
        {"transform", usdPrimTransformJson(prim, xformCache)},
        {"runtimeCameraConverted", true},
        {"projection", orthographic ? "orthographic" : "perspective"},
        {"cameraProjection", orthographic ? 1u : 0u},
        {"cameraYfov", yFov},
        {"cameraAspectRatio", aspect},
        {"cameraOrthoXmag", horizontalAperture},
        {"cameraOrthoYmag", verticalAperture},
        {"cameraNear", clippingRange[0]},
        {"cameraFar", clippingRange[1]},
    };
}

nlohmann::json usdLightRuntimeJson(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache* xformCache = nullptr) {
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
    return {
        {"primPath", prim.GetPath().GetString()},
        {"name", prim.GetName().GetString()},
        {"typeName", typeName},
        {"transform", usdPrimTransformJson(prim, xformCache)},
        {"runtimeLightConverted", true},
        {"lightType", usdLightTypeForTypeName(typeName)},
        {"color", {color[0], color[1], color[2]}},
        {"intensity", intensity},
        {"sizeOrRadius", radius},
        {"innerConeRadians", outerConeRadians * 0.5f},
        {"outerConeRadians", outerConeRadians},
        {"enabled", prim.IsActive()},
    };
}

std::optional<MeshAsset> decodeUsdMeshAsset(
    const pxr::UsdGeomMesh& usdMesh,
    std::string_view fallbackName,
    pxr::UsdGeomXformCache* xformCache,
    nlohmann::json& meshReport,
    std::vector<std::string>& warnings) {
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

    MeshAsset out;
    out.name = prim.GetName().GetString();
    if (out.name.empty()) {
        out.name = std::string(fallbackName.empty() ? "UsdMesh" : fallbackName);
    }
    MeshPrimitiveAsset primitive;
    primitive.firstVertex = 0;
    primitive.firstIndex = 0;
    primitive.alphaMode = kMaterialAlphaModeOpaque;
    primitive.alphaCutoff = 0.5f;

    size_t faceVertexOffset = 0;
    size_t skippedFaces = 0;
    bool generatedAnyNormals = false;
    for (size_t faceIndex = 0; faceIndex < faceVertexCounts.size(); ++faceIndex) {
        const int faceVertexCount = faceVertexCounts[faceIndex];
        if (faceVertexCount < 3 || faceVertexOffset + static_cast<size_t>(faceVertexCount) > faceVertexIndices.size()) {
            faceVertexOffset += static_cast<size_t>(std::max(faceVertexCount, 0));
            ++skippedFaces;
            continue;
        }

        for (int tri = 1; tri + 1 < faceVertexCount; ++tri) {
            const int corners[3] = {0, tri, tri + 1};
            const size_t triangleVertexBase = out.vertices.size();
            bool triangleMissingNormal = false;
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
        }
        faceVertexOffset += static_cast<size_t>(faceVertexCount);
    }

    primitive.vertexCount = static_cast<uint32_t>(out.vertices.size());
    primitive.indexCount = static_cast<uint32_t>(out.indices.size());
    if (out.vertices.empty() || out.indices.empty()) {
        meshReport = {{"primPath", prim.GetPath().GetString()}, {"decoded", false}, {"reason", "no_triangles_decoded"}};
        return std::nullopt;
    }
    out.primitives.push_back(primitive);
    if (skippedFaces > 0) {
        warnings.push_back("USD mesh cook skipped invalid or unsupported faces for prim " + prim.GetPath().GetString() + ".");
    }
    if (generatedAnyNormals) {
        warnings.push_back("USD mesh cook generated fallback face normals for prim " + prim.GetPath().GetString() + ".");
    }
    meshReport = {
        {"primPath", prim.GetPath().GetString()},
        {"name", out.name},
        {"transform", usdPrimTransformJson(prim, xformCache)},
        {"decoded", true},
        {"pointCount", points.size()},
        {"faceCount", faceVertexCounts.size()},
        {"vertexCount", out.vertices.size()},
        {"indexCount", out.indices.size()},
        {"primitiveCount", out.primitives.size()},
        {"skippedFaceCount", skippedFaces},
        {"generatedNormals", generatedAnyNormals},
        {"normalInterpolation", interpolation},
        {"materialBound", usdPrimHasAuthoredMaterialBinding(prim)},
        {"materialBindingPath", materialBindingPath},
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
    pxr::UsdGeomXformCache xformCache(pxr::UsdTimeCode::Default());
    std::unordered_map<std::string, size_t> primIndexByPath;
    size_t primCount = 0;
    size_t xformableCount = 0;
    size_t materialPrimCount = 0;
    for (const pxr::UsdPrim& prim : stage->Traverse()) {
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
        }
        if (isLight) {
            ++out.lightCount;
        }
        if (hasMaterialBinding) {
            ++out.materialBindingCount;
        }
        if (typeName == "Material") {
            ++materialPrimCount;
        }
        const bool isXformable = static_cast<bool>(pxr::UsdGeomXformable(prim));
        if (isXformable) {
            ++xformableCount;
        }

        nlohmann::json primJson = {
            {"index", primCount},
            {"path", path},
            {"name", prim.GetName().GetString()},
            {"parentPath", parentPath},
            {"typeName", typeName},
            {"active", prim.IsActive()},
            {"defined", prim.IsDefined()},
            {"abstract", prim.IsAbstract()},
            {"xformable", isXformable},
            {"transform", usdPrimTransformJson(prim, &xformCache)},
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
        out.warnings.push_back("USDZ package dependencies are inspected through OpenUSD stage metadata only; packaged texture extraction and native texture cooking remain future work.");
    }
    if (out.meshCount > 0) {
        out.warnings.push_back("USD mesh topology metadata was preserved; native .rtmesh cooking runs in the OpenUSD runtime mesh cook stage when topology decodes successfully.");
    }
    if (out.materialBindingCount > 0 || materialPrimCount > 0) {
        out.warnings.push_back("USD material binding metadata was preserved; bound-material .rtmaterial cook uses default material payloads while shader-network texture conversion remains future work.");
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
        {"meshNativeCookImplemented", false},
        {"materialBindingCookImplemented", false},
        {"materialNativeCookImplemented", false},
        {"shaderNetworkConversionImplemented", false},
        {"usdzTextureExtractionImplemented", false},
        {"parentHierarchyTransformCompositionImplemented", true},
        {"viewportPlacementImplemented", false},
        {"prims", out.prims},
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
    pxr::UsdGeomXformCache xformCache(pxr::UsdTimeCode::Default());
    nlohmann::json meshReports = nlohmann::json::array();
    size_t decodedMeshCount = 0;
    size_t decodedVertexCount = 0;
    size_t decodedIndexCount = 0;
    for (const pxr::UsdPrim& prim : stage->Traverse()) {
        if (!prim || !prim.IsA<pxr::UsdGeomMesh>()) {
            continue;
        }
        ++out.meshPrimCount;
        nlohmann::json meshReport = nlohmann::json::object();
        std::optional<MeshAsset> decodedMesh = decodeUsdMeshAsset(pxr::UsdGeomMesh(prim), displayName, &xformCache, meshReport, out.warnings);
        if (!decodedMesh.has_value()) {
            ++out.skippedMeshPrimCount;
            meshReports.push_back(std::move(meshReport));
            continue;
        }
        decodedVertexCount += decodedMesh->vertices.size();
        decodedIndexCount += decodedMesh->indices.size();
        const std::string materialBindingPath = meshReport.value("materialBindingPath", std::string{});
        if (!materialBindingPath.empty()) {
            ++out.materialBindingTargetCount;
        }
        ++decodedMeshCount;
        out.materialBindingPaths.push_back(materialBindingPath);
        out.meshes.push_back(std::move(*decodedMesh));
        meshReports.push_back(std::move(meshReport));
    }

    if (out.meshPrimCount > 0 && out.meshes.empty()) {
        out.errors.push_back("OpenUSD found mesh prims, but no supported triangle topology could be decoded for native .rtmesh cooking.");
    }
    const bool isUsdz = lowerString(sourcePath.extension().string()) == ".usdz";
    if (isUsdz) {
        out.warnings.push_back("USDZ mesh topology can be decoded through OpenUSD, but packaged texture extraction and native texture cooking remain future work.");
    }
    out.diagnostics = {
        {"schema", "UsdRuntimeMeshCookDiagnosticsV1"},
        {"parser", "OpenUSD"},
        {"supported", true},
        {"sourcePath", sourcePath.generic_string()},
        {"meshPrimCount", out.meshPrimCount},
        {"decodedMeshCount", decodedMeshCount},
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
    pxr::UsdGeomXformCache xformCache(pxr::UsdTimeCode::Default());
    for (const pxr::UsdPrim& prim : stage->Traverse()) {
        if (!prim) {
            continue;
        }
        if (prim.IsA<pxr::UsdGeomCamera>()) {
            out.cameras.push_back(usdCameraRuntimeJson(pxr::UsdGeomCamera(prim), &xformCache));
            ++out.cameraCount;
            continue;
        }
        if (usdPrimLooksLikeLight(prim)) {
            out.lights.push_back(usdLightRuntimeJson(prim, &xformCache));
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
        {"runtimeSupport", "decoded_keyframes_playback_system_pending"},
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
                            {"runtimeSupport", "metadata_only_root_motion_extraction_pending"},
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
                {"rootMotionSupport", "metadata_only_root_motion_extraction_pending"},
                {"targetPaths", targetPaths},
                {"clip", {
                    {"startTime", clipStart},
                    {"endTime", clipEnd},
                    {"duration", std::max(0.0, clipEnd - clipStart)},
                    {"runtimeSupport", "metadata_only_playback_pending"},
                }},
            });
        }
    }

    const size_t skinCount = metadata.value("skinCount", 0u);
    const size_t animationCount = metadata.value("animationCount", 0u);
    if (skinCount > 0 || animationCount > 0) {
        warnings.push_back(
            "Source contains skeletal or animation data; skin payloads are preserved for CPU current-pose skinning, while animation playback/cooking remains future work.");
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
        addWarningOnce(warnings, "Source contains collision-named glTF meshes/nodes; collision metadata was preserved, but runtime collision cooking is not implemented yet.");
    }
    if (!lodCandidates.empty()) {
        addWarningOnce(warnings, "Source contains LOD-named glTF meshes/nodes; LOD metadata was preserved, but runtime LOD cooking/selection is not implemented yet.");
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
            addFeature("collisionMetadata", "metadata_only", {
                {"candidateCount", collisionCandidateCount},
                {"candidates", collisionLodMetadata.value("collisionCandidates", nlohmann::json::array())},
                {"runtimeSupport", "collision_cook_pending"},
            });
        }
        if (lodCandidateCount > 0) {
            addFeature("lodMetadata", "metadata_only", {
                {"candidateCount", lodCandidateCount},
                {"candidates", collisionLodMetadata.value("lodCandidates", nlohmann::json::array())},
                {"runtimeSupport", "lod_cook_and_selection_pending"},
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
        size_t morphTargetCount = 0;
        size_t morphPositionTargetCount = 0;
        size_t morphNormalTargetCount = 0;
        size_t morphTangentTargetCount = 0;
        size_t vertexColorPrimitiveCount = 0;
        size_t secondUvPrimitiveCount = 0;
        size_t skinnedPrimitiveCount = 0;
        size_t jointAttributePrimitiveCount = 0;
        size_t weightAttributePrimitiveCount = 0;
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
        if (doc.contains("meshes") && doc["meshes"].is_array()) {
            for (const nlohmann::json& mesh : doc["meshes"]) {
                if (!mesh.contains("primitives") || !mesh["primitives"].is_array()) {
                    continue;
                }
                for (const nlohmann::json& primitive : mesh["primitives"]) {
                    if (primitive.contains("attributes") && primitive["attributes"].is_object()) {
                        const nlohmann::json& attributes = primitive["attributes"];
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
                        const bool hasJoints = attributes.contains("JOINTS_0");
                        const bool hasWeights = attributes.contains("WEIGHTS_0");
                        if (hasJoints) {
                            ++jointAttributePrimitiveCount;
                        }
                        if (hasWeights) {
                            ++weightAttributePrimitiveCount;
                        }
                        if (hasJoints && hasWeights) {
                            ++skinnedPrimitiveCount;
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
                {"attributes", nlohmann::json::array({"JOINTS_0", "WEIGHTS_0"})},
                {"runtimeSupport", "cpu_current_pose_skinning_supported"},
                {"futureWork", "animation_playback_and_gpu_skinning"},
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
                {"runtimeSupport", "cpu_current_pose_skinning_supported"},
                {"futureWork", "animation_playback_and_gpu_skinning"},
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
                {"runtimeSupport", "decoded_keyframes_playback_system_pending"},
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
                {"runtimeSupport", "cpu_authored_weight_evaluation_supported"},
                {"futureWork", "animated_and_interactive_blend_shape_weight_control"},
            });
            addWarningOnce(warnings, "Source contains glTF morph target/blend shape data; authored morph weights are evaluated on CPU for renderer-visible geometry, while animated/interactive blend-shape controls remain future work.");
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
        addFeature("objGeometry", "metadata_only", {
            {"vertexCount", objMetadata.value("vertexCount", 0u)},
            {"faceCount", objMetadata.value("faceCount", 0u)},
            {"runtimeSupport", "native_obj_geometry_cook_pending"},
        });
        addFeature("objMaterialLibraries", "metadata_only", {
            {"materialLibraries", objMetadata.value("materialLibraries", nlohmann::json::array())},
            {"runtimeSupport", "mtl_material_binding_pending"},
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
        const size_t runtimeCameraCount = usdMetadata.value("runtimeCameraCount", 0u);
        const size_t runtimeLightCount = usdMetadata.value("runtimeLightCount", 0u);
        const bool cameraRuntimeConversionImplemented = usdMetadata.value("cameraRuntimeConversionImplemented", false);
        const bool lightRuntimeConversionImplemented = usdMetadata.value("lightRuntimeConversionImplemented", false);
        addFeature("usdStageGraph", meshNativeCookImplemented ? "supported_with_native_mesh_payloads" : "metadata_only", {
            {"parser", "OpenUSD"},
            {"primCount", primCount},
            {"rootPrims", usdMetadata.value("rootPrims", nlohmann::json::array())},
            {"preservedFields", nlohmann::json::array({"primPath", "parentPath", "typeName", "localTransform", "meshAttributes", "materialBindingFlag", "cameraFlag", "lightFlag"})},
            {"runtimeSupport", meshNativeCookImplemented ? "stage_metadata_plus_native_rtmesh_payloads" : "metadata_bridge_only_native_usd_scene_cook_pending"},
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
            {"runtimeSupport", materialNativeCookImplemented ? "usd_material_binding_default_rtmaterial_payloads" : materialBindingCount > 0 ? "usd_material_texture_native_cook_pending" : "no_material_bindings_detected"},
            {"shaderNetworkConversionImplemented", usdMetadata.value("shaderNetworkConversionImplemented", false)},
            {"textureNativeCookImplemented", false},
        });
        addFeature("usdSceneEntities", (cameraRuntimeConversionImplemented || lightRuntimeConversionImplemented) ? "supported_with_runtime_entities" : "metadata_only", {
            {"cameraCount", usdMetadata.value("cameraCount", 0u)},
            {"lightCount", usdMetadata.value("lightCount", 0u)},
            {"runtimeCameraCount", runtimeCameraCount},
            {"runtimeLightCount", runtimeLightCount},
            {"runtimeSupport", (cameraRuntimeConversionImplemented || lightRuntimeConversionImplemented) ? "usd_camera_light_runtime_conversion_supported" : "no_runtime_camera_or_light_entities_detected"},
            {"cameraRuntimeConversionImplemented", cameraRuntimeConversionImplemented},
            {"lightRuntimeConversionImplemented", lightRuntimeConversionImplemented},
            {"viewportPlacementImplemented", false},
        });
        if (ext == ".usdz") {
            addFeature("usdzPackageTextures", "pending_runtime_cook", {
                {"runtimeSupport", "usdz_texture_extraction_and_native_rttexture_cook_pending"},
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
            addFeature(type == AssetType::HDRI ? "environmentImage" : "textureImage", "supported", {{"runtimePayload", "loose_cached_payload"}});
        }
        addFeature("textureRoleDetection", "supported", textureRole);
        if (ext == ".dds" || ext == ".ktx" || ext == ".ktx2") {
            addFeature("compressedTexturePolicy", "metadata_only", {{"policy", "preserve_source_payload_no_platform_transcode"}});
            addWarningOnce(warnings, "Compressed texture source is imported as a loose cached payload; platform transcoding and native texture cook policy remain pending.");
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
    if (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz") return AssetType::Scene;
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

    NativeAssetCooker nativeCooker(workspace.nativeTextureFormatSupport);
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
        {"kind", sourceIsGltf ? "ImportedGltfPrefabRoot" : sourceIsFbx ? "ImportedFbxPrefabRoot" : sourceIsUsd ? "ImportedUsdStage" : sourceIsObj ? "ImportedObjMesh" : sourceIsMtl ? "ImportedMtlMaterialLibrary" : standaloneAssetKindForType(type)},
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
            loader.setNativeTextureFormatSupport(workspace.nativeTextureFormatSupport);
            SceneAsset scene = loader.loadWithCache(effectiveSourcePath);
            result.workerInspectMs = elapsedMilliseconds(inspectStart);
            const bool sceneCacheExists = std::filesystem::exists(sceneCachePath);
            const std::string sceneCacheHash = fileHashString(sceneCachePath);
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
                const nlohmann::json textureRole = inferTextureRole(textureRoleSource, AssetType::Texture);
                const std::string textureThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(texture.sourcePath, workspace.root) : std::string{};
                textureThumbnailPaths.push_back(textureThumbnailPath);
                const NativeAssetCookResult textureCook = nativeCooker.cookTexture(
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
            for (size_t i = 0; i < materials.size(); ++i) {
                const MaterialAsset& material = materials[i];
                const AssetGuid materialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Material", i);
                materialGuids.push_back(materialGuid);
                const std::string materialName = safeStem(material.name.empty() ? ("Material_" + std::to_string(i)) : material.name);
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
                const NativeAssetCookResult materialCook = nativeCooker.cookMaterial(
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
            for (size_t i = 0; i < meshes.size(); ++i) {
                const MeshAsset& mesh = meshes[i];
                const AssetGuid meshGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Mesh", i);
                meshGuids.push_back(meshGuid);
                rootDependencies.push_back(meshGuid);
                const std::string meshName = safeStem((mesh.name.empty() ? "Mesh" : mesh.name) + "_" + std::to_string(i));
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
                const NativeAssetCookResult meshCook = nativeCooker.cookMesh(
                    nativeCookInput(meshGuid, nativeMeshPath, meshName),
                    mesh,
                    materialGuids);
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
                        {"runtimeSupport", "cpu_current_pose_skinning_supported"},
                        {"futureWork", "animation_playback_and_gpu_skinning"},
                    }},
                    {"morphTargets", {
                        {"hasMorphTargets", morphTargetCount > 0},
                        {"targetCount", morphTargetCount},
                        {"primitiveCount", morphTargetPrimitives.size()},
                        {"preservedAttributes", nlohmann::json::array({"POSITION", "NORMAL", "TANGENT"})},
                        {"defaultWeightCount", mesh.defaultMorphWeights.size()},
                        {"defaultWeights", mesh.defaultMorphWeights},
                        {"primitives", morphTargetPrimitives},
                        {"runtimeSupport", "cpu_authored_weight_evaluation_supported"},
                        {"futureWork", "animated_and_interactive_blend_shape_weight_control"},
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
                        {"runtimeSupport", "binding_decode_only_gpu_skinning_runtime_integration_pending"},
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
                    bindingPayload["runtimeSupport"] = "binding_decode_only_gpu_skinning_runtime_integration_pending";
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
                        {"runtimeSupport", "binding_decode_only_gpu_skinning_runtime_integration_pending"},
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
                    animationPayload["runtimeSupport"] = "decoded_keyframes_playback_system_pending";
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
                        {"runtimeSupport", "decoded_keyframes_playback_system_pending"},
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

            cache["runtimePayload"] = runtimePayload;
            cache["cookedPayloads"] = cookedPayloads;
            cache["thumbnail"] = thumbnailMetadata;
            cache["textureCount"] = textures.size();
            cache["materialCount"] = materials.size();
            cache["meshCount"] = meshes.size();
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
        FbxStaticImportData fbxData = loadFbxStaticScene(effectiveSourcePath, name, workspace.nativeTextureFormatSupport);
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
                const NativeAssetCookResult textureCook = nativeCooker.cookTexture(
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
            for (size_t i = 0; i < materials.size(); ++i) {
                const MaterialAsset& material = materials[i];
                const AssetGuid materialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Material", i);
                materialGuids.push_back(materialGuid);
                const std::string materialName = safeStem(material.name.empty() ? ("FbxMaterial_" + std::to_string(i)) : material.name);
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
                const NativeAssetCookResult materialCook = nativeCooker.cookMaterial(
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
            for (size_t i = 0; i < meshes.size(); ++i) {
                const MeshAsset& mesh = meshes[i];
                const AssetGuid meshGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Mesh", i);
                meshGuids.push_back(meshGuid);
                const std::string meshName = safeStem((mesh.name.empty() ? "FbxMesh" : mesh.name) + "_" + std::to_string(i));
                const std::filesystem::path meshPath = importedDir / "Meshes" / (meshName + ".rtmesh.json");
                const std::filesystem::path nativeMeshPath = cacheDir / "Meshes" / (meshName + ".rtmesh");
                std::filesystem::create_directories(meshPath.parent_path(), ec);
                std::filesystem::create_directories(nativeMeshPath.parent_path(), ec);
                const NativeAssetCookResult meshCook = nativeCooker.cookMesh(
                    nativeCookInput(meshGuid, nativeMeshPath, meshName),
                    mesh,
                    materialGuids);
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
                if (primaryMeshCachePath.empty()) {
                    primaryMeshCachePath = meshPayload.value("cachePath", std::string{});
                }
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
                {"runtimeSupport", "metadata_bridge_with_skeletal_mesh_binding_decode_runtime_playback_pending"},
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
                skeletonPayload["runtimeSupport"] = "metadata_bridge_only_runtime_skinning_pending";
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
                    {"runtimeSupport", "metadata_bridge_only_runtime_skinning_pending"},
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
                        {"runtimeSupport", "binding_decode_and_mesh_skin_channels_runtime_playback_pending"},
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
                    bindingPayload["runtimeSupport"] = "binding_decode_and_mesh_skin_channels_runtime_playback_pending";
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
                        {"runtimeSupport", "binding_decode_and_mesh_skin_channels_runtime_playback_pending"},
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
                    {"name", node.name.empty() ? ("FbxNode_" + std::to_string(i)) : node.name},
                    {"parent", node.parent},
                    {"transform", transformJsonFromMatrix(node.transform)},
                    {"matrix", matrixJson(node.transform)},
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
                        workspace.nativeTextureFormatSupport,
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
                    const NativeAssetCookResult textureCook = nativeCooker.cookTexture(
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
        usdMetadata["usdzTextureAssets"] = usdzTextureAssets;

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
            setProgress(0.64f, "Converting USD cameras and lights");
            traceImport("USD import: load scene entities");
            usdSceneEntities = loadUsdSceneEntities(effectiveSourcePath);
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
            std::vector<AssetGuid> usdMeshGuids;
            std::vector<AssetGuid> usdMaterialGuids;
            std::unordered_map<std::string, uint32_t> usdMaterialSlotByPath;
            bool usdShaderNetworkConverted = false;
            bool usdTextureReferenceExtractionImplemented = false;
            bool usdShaderTextureMaterialBindingImplemented = false;
            size_t usdTextureReferenceCount = 0;
            size_t usdShaderTextureBindingCount = 0;
            if (request.settings.importMaterials && usdRuntimeMeshes.supported && !usdRuntimeMeshes.materialBindingPaths.empty()) {
                for (const std::string& materialPath : usdRuntimeMeshes.materialBindingPaths) {
                    if (materialPath.empty() || usdMaterialSlotByPath.find(materialPath) != usdMaterialSlotByPath.end()) {
                        continue;
                    }
                    const uint32_t materialSlot = static_cast<uint32_t>(usdMaterialGuids.size());
                    usdMaterialSlotByPath.emplace(materialPath, materialSlot);
                    const AssetGuid materialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "UsdMaterial", materialSlot);
                    rootDependencies.push_back(materialGuid);
                    usdMaterialGuids.push_back(materialGuid);
                    const std::string materialName = usdMaterialNameFromPath(materialPath, "UsdMaterial") + "_" + std::to_string(materialSlot);
                    const std::filesystem::path materialPathJson = importedDir / "Materials" / (materialName + ".rtmaterial.json");
                    const std::filesystem::path materialCache = cacheDir / "Materials" / (materialName + ".rtmaterial");
                    std::filesystem::create_directories(materialPathJson.parent_path(), ec);
                    std::filesystem::create_directories(materialCache.parent_path(), ec);

                    traceImport("USD import: load material shader network " + materialPath);
                    const UsdMaterialShaderNetworkData shaderNetwork = loadUsdMaterialShaderNetwork(effectiveSourcePath, materialPath, materialName);
                    MaterialAsset material = shaderNetwork.material;
                    usdShaderNetworkConverted = usdShaderNetworkConverted || shaderNetwork.shaderNetworkConverted;
                    usdTextureReferenceExtractionImplemented = usdTextureReferenceExtractionImplemented || shaderNetwork.diagnostics.value("textureReferenceExtractionImplemented", false);
                    usdTextureReferenceCount += shaderNetwork.diagnostics.value("textureReferenceCount", 0u);
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
                            const bool matchedTexture = findTexture(assetPath) || findTexture(resolvedPath);
                            const NativeTextureRole role = usdTextureRoleForReference(reference);
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
                    const NativeAssetCookResult materialCook = nativeCooker.cookMaterial(
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
                    materialPayload["kind"] = "UsdMaterialBindingPayload";
                    materialPayload["materialNativeCookImplemented"] = true;
                    materialPayload["shaderNetworkConversionImplemented"] = shaderNetwork.shaderNetworkConverted;
                    materialPayload["textureReferenceExtractionImplemented"] = shaderNetwork.diagnostics.value("textureReferenceExtractionImplemented", false);
                    materialPayload["textureReferenceCount"] = shaderNetwork.diagnostics.value("textureReferenceCount", 0u);
                    materialPayload["shaderTextureMaterialBindingImplemented"] = materialTextureBindingCount > 0;
                    materialPayload["shaderTextureBindingCount"] = materialTextureBindingCount;
                    materialPayload["textureNativeCookImplemented"] = !materialTextureGuids.empty();
                    materialPayload["textureGuids"] = materialTextureGuids;
                    materialPayload["usdShaderTextureBindings"] = usdShaderTextureBindings;
                    materialPayload["sourceMaterialPath"] = materialPath;
                    materialPayload["usdShaderNetwork"] = shaderNetwork.diagnostics;
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
                            {"shaderTextureMaterialBindingImplemented", materialTextureBindingCount > 0},
                            {"shaderTextureBindingCount", materialTextureBindingCount},
                            {"textureNativeCookImplemented", !materialTextureGuids.empty()},
                            {"usdShaderTextureBindings", usdShaderTextureBindings},
                            {"futureWork", "USD scene placement and broader shader graph texture semantics"},
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
                        {"shaderTextureMaterialBindingImplemented", materialTextureBindingCount > 0},
                        {"shaderTextureBindingCount", materialTextureBindingCount},
                        {"usdShaderTextureBindings", usdShaderTextureBindings},
                        {"usdShaderNetwork", shaderNetwork.diagnostics},
                        {"textureNativeCookImplemented", !materialTextureGuids.empty()},
                    });
                }
            }
            if (usdRuntimeMeshes.supported && !usdRuntimeMeshes.meshes.empty()) {
                traceImport("USD import: cook decoded meshes");
                for (size_t i = 0; i < usdRuntimeMeshes.meshes.size(); ++i) {
                    const MeshAsset& usdMesh = usdRuntimeMeshes.meshes[i];
                    const AssetGuid meshGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "UsdMesh", i);
                    rootDependencies.push_back(meshGuid);
                    usdMeshGuids.push_back(meshGuid);
                    const std::string meshName = safeStem((usdMesh.name.empty() ? "UsdMesh" : usdMesh.name) + "_" + std::to_string(i));
                    const std::filesystem::path meshPath = importedDir / "Meshes" / (meshName + ".rtmesh.json");
                    const std::filesystem::path meshCache = cacheDir / "Meshes" / (meshName + ".rtmesh");
                    std::filesystem::create_directories(meshPath.parent_path(), ec);
                    std::filesystem::create_directories(meshCache.parent_path(), ec);

                    MeshAsset usdMeshForCook = usdMesh;
                    const std::string materialBindingPath = i < usdRuntimeMeshes.materialBindingPaths.size()
                        ? usdRuntimeMeshes.materialBindingPaths[i]
                        : std::string{};
                    const auto materialSlotIt = usdMaterialSlotByPath.find(materialBindingPath);
                    const bool materialSlotBound = materialSlotIt != usdMaterialSlotByPath.end();
                    if (materialSlotBound) {
                        for (MeshPrimitiveAsset& primitive : usdMeshForCook.primitives) {
                            primitive.material = MaterialAssetHandle{materialSlotIt->second};
                        }
                    }

                    const NativeAssetCookResult meshCook = nativeCooker.cookMesh(
                        nativeCookInput(meshGuid, meshCache, meshName),
                        usdMeshForCook,
                        usdMaterialGuids);
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
                    meshPayload["kind"] = "UsdRuntimeMeshPayload";
                    meshPayload["runtimeGeometryCooked"] = true;
                    meshPayload["usdMeshTopologyDecoded"] = true;
                    meshPayload["materialSlotGuidBindingImplemented"] = materialSlotBound;
                    meshPayload["materialGuid"] = materialSlotBound && materialSlotIt->second < usdMaterialGuids.size() ? usdMaterialGuids[materialSlotIt->second] : std::string{};
                    meshPayload["materialBindingPath"] = materialBindingPath;
                    const nlohmann::json usdMeshCookDiagnostics = usdRuntimeMeshes.diagnostics.contains("meshes") && usdRuntimeMeshes.diagnostics["meshes"].is_array() && i < usdRuntimeMeshes.diagnostics["meshes"].size()
                        ? usdRuntimeMeshes.diagnostics["meshes"][i]
                        : nlohmann::json::object();
                    meshPayload["sourcePrimPath"] = usdMeshCookDiagnostics.value("primPath", std::string{});
                    meshPayload["sourcePrimTransform"] = usdMeshCookDiagnostics.value("transform", nlohmann::json::object());
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
                        {"usdRuntimeMeshCook", usdMeshCookDiagnostics},
                        {"materialBinding", {
                            {"materialNativeCookImplemented", materialSlotBound},
                            {"materialSlotGuidBindingImplemented", materialSlotBound},
                            {"materialBindingPath", materialBindingPath},
                            {"materialGuid", materialSlotBound && materialSlotIt->second < usdMaterialGuids.size() ? usdMaterialGuids[materialSlotIt->second] : std::string{}},
                            {"shaderNetworkConversionImplemented", false},
                            {"futureWork", "USD material/shader-network conversion and texture cooking"},
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

                    usdMeshAssets.push_back({
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
                    });
                }
            }
            usdMetadata["runtimeMeshCook"] = usdRuntimeMeshes.diagnostics;
            usdMetadata["meshNativeCookImplemented"] = !usdMeshGuids.empty();
            usdMetadata["meshNativeCookAssetCount"] = usdMeshGuids.size();
            usdMetadata["materialNativeCookImplemented"] = !usdMaterialGuids.empty();
            usdMetadata["materialNativeCookAssetCount"] = usdMaterialGuids.size();
            usdMetadata["materialBindingCookImplemented"] = !usdMaterialGuids.empty();
            usdMetadata["shaderNetworkConversionImplemented"] = usdShaderNetworkConverted;
            usdMetadata["textureReferenceExtractionImplemented"] = usdTextureReferenceExtractionImplemented;
            usdMetadata["textureReferenceCount"] = usdTextureReferenceCount;
            usdMetadata["shaderTextureMaterialBindingImplemented"] = usdShaderTextureMaterialBindingImplemented;
            usdMetadata["shaderTextureBindingCount"] = usdShaderTextureBindingCount;
            usdMetadata["textureNativeCookImplemented"] = usdShaderTextureMaterialBindingImplemented || usdzTextureNativeCookImplemented;
            usdMetadata["runtimeSceneEntities"] = usdSceneEntities.diagnostics;
            usdMetadata["cameraRuntimeConversionImplemented"] = usdSceneEntities.cameraCount > 0;
            usdMetadata["lightRuntimeConversionImplemented"] = usdSceneEntities.lightCount > 0;
            usdMetadata["runtimeCameraCount"] = usdSceneEntities.cameraCount;
            usdMetadata["runtimeLightCount"] = usdSceneEntities.lightCount;

            runtimePayload = {
                {"kind", "UsdStageMetadataPayload"},
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"sourceHash", sourceHash},
                {"importSettingsHash", importSettingsHash},
                {"available", true},
                {"validForSource", true},
                {"sceneGraphMetadataImportImplemented", true},
                {"meshNativeCookImplemented", !usdMeshGuids.empty()},
                {"meshNativeCookAssetCount", usdMeshGuids.size()},
                {"meshAssets", usdMeshAssets},
                {"materialNativeCookImplemented", !usdMaterialGuids.empty()},
                {"materialNativeCookAssetCount", usdMaterialGuids.size()},
                {"materialBindingCookImplemented", !usdMaterialGuids.empty()},
                {"materialAssets", usdMaterialAssets},
                {"shaderNetworkConversionImplemented", usdShaderNetworkConverted},
                {"textureReferenceExtractionImplemented", usdTextureReferenceExtractionImplemented},
                {"textureReferenceCount", usdTextureReferenceCount},
                {"shaderTextureMaterialBindingImplemented", usdShaderTextureMaterialBindingImplemented},
                {"shaderTextureBindingCount", usdShaderTextureBindingCount},
                {"textureNativeCookImplemented", usdShaderTextureMaterialBindingImplemented || usdzTextureNativeCookImplemented},
                {"usdzTextureProvenanceInspectionImplemented", usdzPackageTextures.inspected},
                {"usdzPackagedTextureEntryCount", usdzPackageTextures.textureEntryCount},
                {"usdzTextureExtractionImplemented", usdzTextureExtractionImplemented},
                {"usdzTextureNativeCookImplemented", usdzTextureNativeCookImplemented},
                {"usdzExtractedTextureCount", usdzExtractedTextureCount},
                {"usdzNativeTextureCookCount", usdzNativeTextureCookCount},
                {"usdzTextureAssets", usdzTextureAssets},
                {"usdzPackageTextures", usdzPackageTextures.diagnostics},
                {"cameraRuntimeConversionImplemented", usdSceneEntities.cameraCount > 0},
                {"lightRuntimeConversionImplemented", usdSceneEntities.lightCount > 0},
                {"runtimeCameras", usdSceneEntities.cameras},
                {"runtimeLights", usdSceneEntities.lights},
                {"viewportPlacementImplemented", false},
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
                }},
            };
            cookedPayloads.push_back(runtimePayload);
            placeholder["runtimePayload"] = runtimePayload;
            placeholder["thumbnail"] = thumbnailMetadata;
            placeholder["usdMetadata"] = usdMetadata;
            placeholder["sourceHierarchy"] = usdData.prims;
            placeholder["rootPrims"] = usdData.rootPrims;
            placeholder["primCount"] = usdData.prims.size();
            placeholder["meshCount"] = usdData.meshCount;
            placeholder["nativeMeshCount"] = usdMeshGuids.size();
            placeholder["meshAssets"] = usdMeshAssets;
            placeholder["nativeMaterialCount"] = usdMaterialGuids.size();
            placeholder["materialAssets"] = usdMaterialAssets;
            placeholder["materialBindingCount"] = usdData.materialBindingCount;
            placeholder["cameraCount"] = usdData.cameraCount;
            placeholder["lightCount"] = usdData.lightCount;
            placeholder["runtimeCameras"] = usdSceneEntities.cameras;
            placeholder["runtimeLights"] = usdSceneEntities.lights;
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
            placeholder["shaderTextureMaterialBindingImplemented"] = usdShaderTextureMaterialBindingImplemented;
            placeholder["shaderTextureBindingCount"] = usdShaderTextureBindingCount;
            placeholder["textureNativeCookImplemented"] = usdShaderTextureMaterialBindingImplemented || usdzTextureNativeCookImplemented;
            placeholder["usdRuntimeSceneEntities"] = usdSceneEntities.diagnostics;
            placeholder["usdzTextureProvenanceInspectionImplemented"] = usdzPackageTextures.inspected;
            placeholder["usdzPackagedTextureEntryCount"] = usdzPackageTextures.textureEntryCount;
            placeholder["usdzTextureExtractionImplemented"] = usdzTextureExtractionImplemented;
            placeholder["usdzTextureNativeCookImplemented"] = usdzTextureNativeCookImplemented;
            placeholder["usdzExtractedTextureCount"] = usdzExtractedTextureCount;
            placeholder["usdzNativeTextureCookCount"] = usdzNativeTextureCookCount;
            placeholder["usdzTextureAssets"] = usdzTextureAssets;
            placeholder["usdzPackageTextures"] = usdzPackageTextures.diagnostics;
            placeholder["viewportPlacementImplemented"] = false;
            placeholder["sourceExtension"] = sourceExtension;
            placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
            placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
            cache["runtimePayload"] = runtimePayload;
            cache["cookedPayloads"] = cookedPayloads;
            cache["thumbnail"] = thumbnailMetadata;
            cache["usdMetadata"] = usdMetadata;
            cache["primCount"] = usdData.prims.size();
            cache["meshCount"] = usdData.meshCount;
            cache["nativeMeshCount"] = usdMeshGuids.size();
            cache["meshAssets"] = usdMeshAssets;
            cache["nativeMaterialCount"] = usdMaterialGuids.size();
            cache["materialAssets"] = usdMaterialAssets;
            cache["textureReferenceExtractionImplemented"] = usdTextureReferenceExtractionImplemented;
            cache["textureReferenceCount"] = usdTextureReferenceCount;
            cache["shaderTextureMaterialBindingImplemented"] = usdShaderTextureMaterialBindingImplemented;
            cache["shaderTextureBindingCount"] = usdShaderTextureBindingCount;
            cache["textureNativeCookImplemented"] = usdShaderTextureMaterialBindingImplemented || usdzTextureNativeCookImplemented;
            cache["usdzTextureProvenanceInspectionImplemented"] = usdzPackageTextures.inspected;
            cache["usdzPackagedTextureEntryCount"] = usdzPackageTextures.textureEntryCount;
            cache["usdzTextureExtractionImplemented"] = usdzTextureExtractionImplemented;
            cache["usdzTextureNativeCookImplemented"] = usdzTextureNativeCookImplemented;
            cache["usdzExtractedTextureCount"] = usdzExtractedTextureCount;
            cache["usdzNativeTextureCookCount"] = usdzNativeTextureCookCount;
            cache["usdzTextureAssets"] = usdzTextureAssets;
            cache["usdzPackageTextures"] = usdzPackageTextures.diagnostics;
            cache["materialBindingCount"] = usdData.materialBindingCount;
            cache["cameraCount"] = usdData.cameraCount;
            cache["lightCount"] = usdData.lightCount;
            cache["runtimeCameraCount"] = usdSceneEntities.cameraCount;
            cache["runtimeLightCount"] = usdSceneEntities.lightCount;
            cache["runtimeCameras"] = usdSceneEntities.cameras;
            cache["runtimeLights"] = usdSceneEntities.lights;
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
                    textureData = TextureLoader::load(textureSourcePath.string(), workspace.nativeTextureFormatSupport, nativeRole, colorSpace);
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

                const NativeAssetCookResult textureCook = nativeCooker.cookTexture(
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

            auto assignObjMtlTexture = [&](MaterialAsset& material, const std::filesystem::path& mtlSourcePath, const nlohmann::json& textureMaps, const char* mtlKey, nlohmann::json& textureDependencies, nlohmann::json& unboundTextures) {
                const nlohmann::json* mapValue = jsonObjectValueCaseInsensitive(textureMaps, mtlKey);
                if (mapValue == nullptr || !mapValue->is_string()) {
                    return;
                }
                const std::string textureToken = mtlTexturePathValueToken(mapValue->get<std::string>());
                const std::string role = mtlTextureRoleStringForKey(mtlKey);
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
                    textureDependencies.push_back({{"guid", textureGuid}, {"role", role}, {"mtlKey", mtlKey}});
                } else {
                    unboundTextures.push_back({{"guid", textureGuid}, {"role", role}, {"mtlKey", mtlKey}, {"reason", "native-material-slot-not-yet-defined"}});
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
                        for (const char* key : {"map_Kd", "map_Ke", "map_Ks", "map_Ns", "map_Pr", "map_Pm", "map_bump", "bump", "map_d", "disp"}) {
                            assignObjMtlTexture(material, mtlSourcePath, textureMaps, key, textureDependencies, unboundTextures);
                        }
                    }
                    const AssetGuid materialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "ObjMtlMaterial", materialId);
                    if (materialId >= objMaterialGuids.size()) {
                        objMaterialGuids.resize(materialId + 1u);
                    }
                    objMaterialGuids[materialId] = materialGuid;
                    rootDependencies.push_back(materialGuid);
                    const std::filesystem::path materialPath = importedDir / "Materials" / (materialName + ".rtmaterial.json");
                    const std::filesystem::path nativeMaterialPath = cacheDir / "Materials" / (materialName + ".rtmaterial");
                    std::filesystem::create_directories(materialPath.parent_path(), ec);
                    std::filesystem::create_directories(nativeMaterialPath.parent_path(), ec);
                    const NativeAssetCookResult materialCook = nativeCooker.cookMaterial(
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
            const NativeAssetCookResult meshCook = nativeCooker.cookMesh(
                nativeCookInput(guid, nativeMeshPath, name),
                objMeshForCook,
                objMaterialGuids);
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
            runtimePayload["objMaterialSlotGuidBindingImplemented"] = !objMaterialGuids.empty();
            runtimePayload["objLinkedMtlTextureBindingImplemented"] = !objTextureGuids.empty();
            runtimePayload["materialSlotCount"] = objMaterialGuids.size();
            runtimePayload["materialAssets"] = objMtlMaterialAssets;
            runtimePayload["textureAssets"] = objMtlTextureAssets;
            runtimePayload["textureCount"] = objTextureGuids.size();
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
                    {"futureWork", "OBJ viewport placement"},
                }},
            });
            placeholder["runtimeGeometryCooked"] = true;
            placeholder["objMaterialSlotGuidBindingImplemented"] = !objMaterialGuids.empty();
            placeholder["objLinkedMtlTextureBindingImplemented"] = !objTextureGuids.empty();
            placeholder["materialAssets"] = objMtlMaterialAssets;
            placeholder["textureAssets"] = objMtlTextureAssets;
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
                textureData = TextureLoader::load(textureSourcePath.string(), workspace.nativeTextureFormatSupport, nativeRole, colorSpace);
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

            const NativeAssetCookResult textureCook = nativeCooker.cookTexture(
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

        auto assignMtlTexture = [&](MaterialAsset& material, const nlohmann::json& textureMaps, const char* mtlKey, nlohmann::json& textureDependencies, nlohmann::json& unboundTextures) {
            const nlohmann::json* mapValue = jsonObjectValueCaseInsensitive(textureMaps, mtlKey);
            if (mapValue == nullptr || !mapValue->is_string()) {
                return;
            }
            const std::string textureToken = mtlTexturePathValueToken(mapValue->get<std::string>());
            const std::string role = mtlTextureRoleStringForKey(mtlKey);
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
                textureDependencies.push_back({{"guid", textureGuid}, {"role", role}, {"mtlKey", mtlKey}});
            } else {
                unboundTextures.push_back({{"guid", textureGuid}, {"role", role}, {"mtlKey", mtlKey}, {"reason", "native-material-slot-not-yet-defined"}});
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
            for (const char* key : {"map_Kd", "map_Ke", "map_Ks", "map_Ns", "map_Pr", "map_Pm", "map_bump", "bump", "map_d", "disp"}) {
                assignMtlTexture(material, textureMaps, key, textureDependencies, unboundTextures);
            }

            if (!request.settings.importMaterials) {
                continue;
            }
            const AssetGuid materialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "MtlMaterial", materialGuids.size());
            materialGuids.push_back(materialGuid);
            rootDependencies.push_back(materialGuid);
            const std::filesystem::path materialPath = importedDir / "Materials" / (materialName + ".rtmaterial.json");
            const std::filesystem::path nativeMaterialPath = cacheDir / "Materials" / (materialName + ".rtmaterial");
            std::filesystem::create_directories(materialPath.parent_path(), ec);
            std::filesystem::create_directories(nativeMaterialPath.parent_path(), ec);
            const NativeAssetCookResult materialCook = nativeCooker.cookMaterial(
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
        placeholder["sourceExtension"] = sourceExtension;
        placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
        placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
        cache["runtimePayload"] = runtimePayload;
        cache["cookedPayloads"] = cookedPayloads;
        cache["thumbnail"] = thumbnailMetadata;
        cache["mtlMetadata"] = mtlMetadata;
        cache["materialAssets"] = generatedMaterials;
        cache["textureAssets"] = generatedTextures;
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
                    workspace.nativeTextureFormatSupport,
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
                {"payloadHash", fileHashString(payloadPath)},
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
