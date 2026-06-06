#include "rtv/AssetImport.h"

#include "rtv/AssetManager.h"
#include "rtv/GltfLoader.h"
#include "rtv/SceneCache.h"
#include "rtv/TextureLoader.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <glm/gtc/quaternion.hpp>

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
    case AssetType::Skeleton: return "Skeletons";
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
    case AssetType::Skeleton: return ".rtskeleton.json";
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
    case AssetType::Skeleton: return "ImportedSkeleton";
    default: return "PlaceholderImportedAsset";
    }
}

std::string importerLabelForType(AssetType type) {
    switch (type) {
    case AssetType::Prefab: return "GltfLoader";
    case AssetType::Texture: return "TextureLoader";
    case AssetType::HDRI: return "HDRTextureLoader";
    case AssetType::Material: return "MtlMetadataImporter";
    default: return "PlaceholderImporter";
    }
}

std::string importCacheKindForType(AssetType type) {
    switch (type) {
    case AssetType::Prefab: return "GltfImportCacheSummary";
    case AssetType::Texture: return "TextureImportCacheSummary";
    case AssetType::HDRI: return "HDRIImportCacheSummary";
    case AssetType::Material: return "MtlImportCacheSummary";
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
        {"workflow", "glTF metallic-roughness"},
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
            {"role", "environment"},
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
        "KHR_materials_sheen",
        "KHR_materials_iridescence",
        "KHR_materials_emissive_strength",
        "KHR_materials_anisotropy",
        "KHR_materials_unlit",
        "KHR_materials_variants",
        "KHR_mesh_quantization",
        "KHR_texture_basisu",
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
    return key.rfind("map_", 0) == 0 || key == "bump" || key == "disp" || key == "decal";
}

std::string mtlTextureMapRoleLabel(const std::string& key) {
    if (key == "map_Kd") return "Base color texture";
    if (key == "map_Ks") return "Specular color texture";
    if (key == "map_Ns") return "Specular exponent texture";
    if (key == "map_d") return "Opacity texture";
    if (key == "map_bump" || key == "bump") return "Normal/bump texture";
    if (key == "disp") return "Displacement texture";
    if (key == "decal") return "Stencil/decal texture";
    if (key.rfind("map_", 0) == 0) return "Texture map";
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
    const nlohmann::json& skeletalAnimationMetadata,
    const nlohmann::json& collisionLodMetadata,
    std::vector<std::string>& warnings) {
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
        addFeature("mtlMaterials", "metadata_only", {
            {"materialCount", mtlMetadata.value("materialCount", 0u)},
            {"textureMapCount", mtlMetadata.value("textureMapCount", 0u)},
            {"runtimeSupport", "native_material_cook_pending"},
        });
        addFeature("mtlTextureReferences", "metadata_only", {
            {"textureReferences", mtlMetadata.value("textureReferences", nlohmann::json::array())},
            {"runtimeSupport", "texture_binding_pending"},
        });
    } else if (type == AssetType::Texture || type == AssetType::HDRI) {
        const nlohmann::json textureRole = inferTextureRole(sourcePath, type);
        addFeature(type == AssetType::HDRI ? "environmentImage" : "textureImage", "supported", {{"runtimePayload", "loose_cached_payload"}});
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
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".dds" || ext == ".ktx" || ext == ".ktx2") return AssetType::Texture;
    if (ext == ".rtlevel") return AssetType::Scene;
    if (ext == ".gltf" || ext == ".glb") return AssetType::Prefab;
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
        {"kind", sourceIsGltf ? "ImportedGltfPrefabRoot" : sourceIsObj ? "ImportedObjMesh" : sourceIsMtl ? "ImportedMtlMaterialLibrary" : standaloneAssetKindForType(type)},
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
        {"kind", sourceIsObj ? "ObjImportCacheSummary" : sourceIsMtl ? "MtlImportCacheSummary" : importCacheKindForType(type)},
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
    nlohmann::json importerCapabilities = nlohmann::json::object();
    nlohmann::json textureRoleMetadata = nlohmann::json::object();
    nlohmann::json thumbnailMetadata = nlohmann::json::object();
    std::string rootThumbnailPath;

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
                const nlohmann::json texturePayload = sceneCacheSlicePayload("Texture", i, textureGuid);
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
                std::string materialThumbnailPath;
                if (request.settings.generateThumbnails) {
                    materialThumbnailPath = thumbnailForTexture(material.baseColorTexture);
                    if (materialThumbnailPath.empty()) materialThumbnailPath = thumbnailForTexture(material.emissiveTexture);
                    if (materialThumbnailPath.empty()) materialThumbnailPath = thumbnailForTexture(material.normalTexture);
                    if (materialThumbnailPath.empty()) materialThumbnailPath = thumbnailForTexture(material.metallicRoughnessTexture);
                }
                const nlohmann::json pbrMetadata = materialPbrMetadataJson(material);
                const nlohmann::json materialPayload = sceneCacheSlicePayload("Material", i, materialGuid);
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
                const std::string meshName = safeStem(mesh.name.empty() ? ("Mesh_" + std::to_string(i)) : mesh.name);
                const std::filesystem::path meshPath = importedDir / "Meshes" / (meshName + ".rtmesh.json");
                const std::filesystem::path meshCache = cacheDir / "Meshes" / (meshName + ".rtmeshcache.json");
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
                const nlohmann::json meshPayload = sceneCacheSlicePayload("Mesh", i, meshGuid);
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
            if (skeletalAnimationMetadata.contains("skins") && skeletalAnimationMetadata["skins"].is_array()) {
                for (size_t i = 0; i < skeletalAnimationMetadata["skins"].size(); ++i) {
                    const nlohmann::json& skin = skeletalAnimationMetadata["skins"][i];
                    const AssetGuid skeletonGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Skeleton", i);
                    rootDependencies.push_back(skeletonGuid);
                    const std::string authoredSkeletonName = skin.value("name", std::string{});
                    const std::string skeletonName = safeStem(authoredSkeletonName.empty() ? ("Skeleton_" + std::to_string(i)) : authoredSkeletonName);
                    const std::filesystem::path skeletonPath = importedDir / "Skeletons" / (skeletonName + ".rtskeleton.json");
                    std::filesystem::create_directories(skeletonPath.parent_path(), ec);
                    const nlohmann::json skeletonPayload = {
                        {"kind", "GltfSkeletonMetadata"},
                        {"cachePath", genericRelativeOrValue(skeletonPath, workspace.root)},
                        {"assetKind", "Skeleton"},
                        {"assetIndex", i},
                        {"assetGuid", skeletonGuid},
                        {"sourcePath", effectiveSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"payloadHash", "metadata_only"},
                        {"available", true},
                        {"runtimeSupport", "cpu_current_pose_skinning_supported"},
                    };
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
            if (skeletalAnimationMetadata.contains("animations") && skeletalAnimationMetadata["animations"].is_array()) {
                for (size_t i = 0; i < skeletalAnimationMetadata["animations"].size(); ++i) {
                    const nlohmann::json& animation = skeletalAnimationMetadata["animations"][i];
                    const AssetGuid animationGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Animation", i);
                    rootDependencies.push_back(animationGuid);
                    const std::string authoredAnimationName = animation.value("name", std::string{});
                    const std::string animationName = safeStem(authoredAnimationName.empty() ? ("Animation_" + std::to_string(i)) : authoredAnimationName);
                    const std::filesystem::path animationPath = importedDir / "Animations" / (animationName + ".rtanim.json");
                    std::filesystem::create_directories(animationPath.parent_path(), ec);
                    const nlohmann::json animationPayload = {
                        {"kind", "GltfAnimationClip"},
                        {"cachePath", genericRelativeOrValue(animationPath, workspace.root)},
                        {"assetKind", "Animation"},
                        {"assetIndex", i},
                        {"assetGuid", animationGuid},
                        {"sourcePath", effectiveSourceString},
                        {"sourceHash", sourceHash},
                        {"importSettingsHash", importSettingsHash},
                        {"payloadHash", "transparent_decoded_keyframes"},
                        {"available", true},
                        {"runtimeSupport", "decoded_keyframes_playback_system_pending"},
                    };
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
            cache["collisionLodMetadata"] = collisionLodMetadata;
        } catch (const std::exception& ex) {
            if (result.workerInspectMs <= 0.0) {
                result.workerInspectMs = elapsedMilliseconds(workerStart) - result.workerValidateMs - result.workerDirectoryMs;
            }
            result.errors.push_back(std::string("glTF import inspection failed: ") + ex.what());
            result.workerTotalMs = elapsedMilliseconds(workerStart);
            return result;
        }
    } else if (sourceIsObj) {
        const auto inspectStart = std::chrono::steady_clock::now();
        setProgress(0.45f, "Inspecting OBJ source");
        objMetadata = inspectObjSource(effectiveSourcePath, result.warnings);
        result.workerInspectMs = elapsedMilliseconds(inspectStart);
        rootThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(effectiveSourcePath, workspace.root) : std::string{};
        thumbnailMetadata = thumbnailMetadataJson("GeneratedSourcePreview", rootThumbnailPath, sourceHash, importSettingsHash);

        result.warnings.push_back("OBJ import currently preserves inspectable mesh/material-library metadata only; native OBJ geometry cooking and placement remain pending.");
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
        };
        cookedPayloads.push_back(runtimePayload);
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

        result.warnings.push_back("MTL import currently preserves inspectable material metadata only; native material asset cooking and texture binding remain pending.");
        runtimePayload = {
            {"kind", "MtlMetadataOnly"},
            {"sourcePath", effectiveSourceString},
            {"originalSourcePath", originalSourceString},
            {"copiedSourcePath", copiedSourceString},
            {"sourceHash", sourceHash},
            {"importSettingsHash", importSettingsHash},
            {"available", std::filesystem::exists(effectiveSourcePath)},
            {"validForSource", true},
            {"runtimeMaterialCooked", false},
            {"metadata", mtlMetadata},
        };
        cookedPayloads.push_back(runtimePayload);
        placeholder["runtimePayload"] = runtimePayload;
        placeholder["thumbnail"] = thumbnailMetadata;
        placeholder["mtlMetadata"] = mtlMetadata;
        placeholder["sourceExtension"] = sourceExtension;
        placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
        placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
        cache["runtimePayload"] = runtimePayload;
        cache["cookedPayloads"] = cookedPayloads;
        cache["thumbnail"] = thumbnailMetadata;
        cache["mtlMetadata"] = mtlMetadata;
    } else if (sourceIsStandaloneTexture) {
        const auto inspectStart = std::chrono::steady_clock::now();
        setProgress(0.45f, type == AssetType::HDRI ? "Inspecting HDRI source" : "Inspecting texture source");
        textureRoleMetadata = inferTextureRole(effectiveSourcePath, type);

        const std::filesystem::path payloadPath = cacheDir / (name + effectiveSourcePath.extension().string());
        std::filesystem::copy_file(effectiveSourcePath, payloadPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            result.errors.push_back("Could not copy texture payload to cache: " + ec.message());
            result.workerInspectMs = elapsedMilliseconds(inspectStart);
            result.workerTotalMs = elapsedMilliseconds(workerStart);
            return result;
        }
        result.generatedFiles.push_back(payloadPath);
        rootThumbnailPath = request.settings.generateThumbnails ? projectRelativePathOrEmpty(payloadPath, workspace.root) : std::string{};

        bool inspected = false;
        TextureData textureData;
        try {
            textureData = TextureLoader::load(effectiveSourcePath.string());
            inspected = true;
        } catch (const std::exception& ex) {
            result.warnings.push_back("Texture dimension/format inspection failed; metadata records file payload only: " + std::string(ex.what()));
        }
        result.workerInspectMs = elapsedMilliseconds(inspectStart);

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
            {"importer", sourceIsObj ? "ObjMetadataImporter" : sourceIsMtl ? "MtlMetadataImporter" : importerLabelForType(type)},
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
    record.status = AssetImportStatus::Imported;
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
    result.success = true;
    setProgress(1.0f, "Import staged");
    return result;
}

} // namespace rtv
