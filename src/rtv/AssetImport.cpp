#include "rtv/AssetImport.h"

#include "rtv/AssetManager.h"
#include "rtv/GltfLoader.h"
#include "rtv/SceneCache.h"
#include "rtv/TextureLoader.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <exception>
#include <unordered_set>
#include <vector>

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
    case AssetType::Unknown:
    default: return ".rtasset.json";
    }
}

std::string standaloneAssetKindForType(AssetType type) {
    switch (type) {
    case AssetType::Texture: return "ImportedTexture";
    case AssetType::HDRI: return "ImportedHDRI";
    case AssetType::Material: return "ImportedMtlMaterialLibrary";
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
        "KHR_materials_ior",
        "KHR_materials_specular",
        "KHR_materials_sheen",
        "KHR_materials_iridescence",
        "KHR_materials_emissive_strength",
        "KHR_materials_anisotropy",
        "KHR_lights_punctual",
        "KHR_texture_transform",
    };
    return supported.find(name) != supported.end();
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
            continue;
        }
        if (currentMaterial.is_null() || !currentMaterial.is_object()) {
            currentMaterial = {{"name", "DefaultMaterial"}, {"properties", nlohmann::json::object()}, {"textureMaps", nlohmann::json::object()}};
        }
        if (key.rfind("map_", 0) == 0 || key == "bump" || key == "disp" || key == "decal") {
            const std::string texturePath = mtlTexturePathToken(tokens);
            currentMaterial["textureMaps"][key] = trimAscii(std::string_view(trimmed).substr(key.size()));
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

std::string animationTargetPathLabel(const nlohmann::json& channel, const nlohmann::json& doc) {
    if (!channel.is_object() || !channel.contains("target") || !channel["target"].is_object()) {
        return "unknown";
    }
    const nlohmann::json& target = channel["target"];
    std::string path = target.value("path", std::string("unknown"));
    if (target.contains("node") && target["node"].is_number_integer()) {
        const int nodeIndex = target["node"].get<int>();
        if (doc.contains("nodes") && doc["nodes"].is_array() && nodeIndex >= 0 && static_cast<size_t>(nodeIndex) < doc["nodes"].size()) {
            const std::string nodeName = doc["nodes"][static_cast<size_t>(nodeIndex)].value("name", std::string{});
            if (!nodeName.empty()) {
                path += "@" + nodeName;
            }
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

    if (doc.contains("skins") && doc["skins"].is_array()) {
        metadata["skinCount"] = doc["skins"].size();
        for (size_t i = 0; i < doc["skins"].size(); ++i) {
            const nlohmann::json& skin = doc["skins"][i];
            const size_t jointCount = skin.contains("joints") && skin["joints"].is_array() ? skin["joints"].size() : 0;
            metadata["skins"].push_back({
                {"index", i},
                {"name", skin.value("name", std::string{})},
                {"jointCount", jointCount},
                {"hasSkeletonRoot", skin.contains("skeleton")},
                {"hasInverseBindMatrices", skin.contains("inverseBindMatrices")},
            });
        }
    }

    if (doc.contains("animations") && doc["animations"].is_array()) {
        metadata["animationCount"] = doc["animations"].size();
        for (size_t i = 0; i < doc["animations"].size(); ++i) {
            const nlohmann::json& animation = doc["animations"][i];
            nlohmann::json targetPaths = nlohmann::json::array();
            std::unordered_set<std::string> uniqueTargetPaths;
            if (animation.contains("channels") && animation["channels"].is_array()) {
                for (const nlohmann::json& channel : animation["channels"]) {
                    const std::string path = animationTargetPathLabel(channel, doc);
                    if (uniqueTargetPaths.insert(path).second) {
                        targetPaths.push_back(path);
                    }
                }
            }
            metadata["animations"].push_back({
                {"index", i},
                {"name", animation.value("name", std::string{})},
                {"channelCount", animation.contains("channels") && animation["channels"].is_array() ? animation["channels"].size() : 0},
                {"samplerCount", animation.contains("samplers") && animation["samplers"].is_array() ? animation["samplers"].size() : 0},
                {"targetPaths", targetPaths},
            });
        }
    }

    const size_t skinCount = metadata.value("skinCount", 0u);
    const size_t animationCount = metadata.value("animationCount", 0u);
    if (skinCount > 0 || animationCount > 0) {
        warnings.push_back(
            "Source contains skeletal or animation data; metadata was preserved in the import report, but runtime skeletal playback/cooking is not implemented yet.");
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
        } else if (supportValue == "unsupported") {
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
        const size_t cameraCount = doc.contains("cameras") && doc["cameras"].is_array() ? doc["cameras"].size() : 0;
        size_t lightCount = 0;
        if (doc.contains("extensions") && doc["extensions"].is_object()) {
            const nlohmann::json& extensions = doc["extensions"];
            if (extensions.contains("KHR_lights_punctual") && extensions["KHR_lights_punctual"].contains("lights") &&
                extensions["KHR_lights_punctual"]["lights"].is_array()) {
                lightCount = extensions["KHR_lights_punctual"]["lights"].size();
            }
        }

        size_t morphTargetPrimitiveCount = 0;
        size_t morphTargetCount = 0;
        if (doc.contains("meshes") && doc["meshes"].is_array()) {
            for (const nlohmann::json& mesh : doc["meshes"]) {
                if (!mesh.contains("primitives") || !mesh["primitives"].is_array()) {
                    continue;
                }
                for (const nlohmann::json& primitive : mesh["primitives"]) {
                    if (primitive.contains("targets") && primitive["targets"].is_array() && !primitive["targets"].empty()) {
                        ++morphTargetPrimitiveCount;
                        morphTargetCount += primitive["targets"].size();
                    }
                }
            }
        }

        addFeature("meshes", "supported", {{"count", meshCount}});
        addFeature("materials", settings.importMaterials ? "supported" : "disabled_by_import_settings", {{"count", materialCount}});
        addFeature("textures", settings.importTextures ? "supported" : "disabled_by_import_settings", {{"count", textureCount}});
        addFeature("cameras", settings.importCameras ? "supported" : "disabled_by_import_settings", {{"count", cameraCount}});
        addFeature("punctualLights", settings.importLights ? "supported" : "disabled_by_import_settings", {{"count", lightCount}});

        const size_t skinCount = skeletalAnimationMetadata.value("skinCount", 0u);
        const size_t animationCount = skeletalAnimationMetadata.value("animationCount", 0u);
        if (skinCount > 0) {
            addFeature("skins", "metadata_only", {{"count", skinCount}, {"runtimeSupport", "not_cooked_for_runtime_playback"}});
        }
        if (animationCount > 0) {
            addFeature("animations", "metadata_only", {{"count", animationCount}, {"runtimeSupport", "not_cooked_for_runtime_playback"}});
        }
        if (morphTargetCount > 0) {
            addFeature("morphTargets", "unsupported", {
                {"targetCount", morphTargetCount},
                {"primitiveCount", morphTargetPrimitiveCount},
                {"reason", "runtime morph target cooking and playback are not implemented"},
            });
            addWarningOnce(warnings, "Source contains glTF morph target/blend shape data; this importer reports it as unsupported because runtime morph target cooking is not implemented yet.");
        }
        addCollisionLodFeatures();

        report["extensionsUsed"] = jsonStringArray(doc, "extensionsUsed");
        report["extensionsRequired"] = jsonStringArray(doc, "extensionsRequired");
        nlohmann::json unsupportedExtensions = nlohmann::json::array();
        auto inspectExtensionList = [&](const nlohmann::json& extensions, bool required) {
            for (const nlohmann::json& item : extensions) {
                if (!item.is_string()) {
                    continue;
                }
                const std::string extension = item.get<std::string>();
                if (!supportedGltfExtensionForImportReport(extension)) {
                    unsupportedExtensions.push_back({{"name", extension}, {"required", required}});
                    addWarningOnce(warnings, std::string("glTF import report: unsupported ") + (required ? "required " : "") + "extension '" + extension + "'.");
                }
            }
        };
        inspectExtensionList(report["extensionsUsed"], false);
        inspectExtensionList(report["extensionsRequired"], true);
        if (!unsupportedExtensions.empty()) {
            addFeature("extensions", "unsupported", {{"unsupported", unsupportedExtensions}});
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
        << ':' << request.settings.unitScale
        << ':' << request.settings.coordinateConversion;
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

    std::vector<AssetRecord> records;
    std::vector<AssetGuid> rootDependencies;
    if (sourceIsGltf) {
        try {
            const auto inspectStart = std::chrono::steady_clock::now();
            setProgress(0.35f, "Inspecting and cooking glTF source");
            const std::filesystem::path sceneCachePath = SceneCache::cachePathFor(effectiveSourcePath);
            const bool sceneCacheWasValidBefore = SceneCache::isCacheValid(effectiveSourcePath, sceneCachePath);
            AssetManager importedAssets;
            GltfLoader loader(importedAssets);
            SceneAsset scene = loader.loadWithCache(effectiveSourcePath);
            result.workerInspectMs = elapsedMilliseconds(inspectStart);
            const bool sceneCacheExists = std::filesystem::exists(sceneCachePath);
            const std::string sceneCacheHash = fileHashString(sceneCachePath);
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
                const std::string textureSourcePath = texture.sourcePath.empty() ? effectiveSourceString : texture.sourcePath.generic_string();
                const nlohmann::json texturePayload = sceneCacheSlicePayload("Texture", i, textureGuid);
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
                record.sourcePath = textureSourcePath;
                record.importedPath = genericRelativeOrValue(texturePath, workspace.root);
                record.cachePath = texturePayload.value("cachePath", genericRelativeOrValue(textureCache, workspace.root));
                record.sourceHash = sourceHash;
                record.importSettingsHash = importSettingsHash;
                record.lastModifiedTimestamp = timestampString();
                record.importSettings = request.settings;
                record.status = AssetImportStatus::Imported;
                records.push_back(std::move(record));
            }

            setProgress(0.68f, "Writing material metadata");
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
                addTextureDependency(material.clearcoatTexture, "clearcoat", "Linear");
                addTextureDependency(material.clearcoatRoughnessTexture, "clearcoatRoughness", "Linear");
                addTextureDependency(material.clearcoatNormalTexture, "clearcoatNormal", "Linear");
                addTextureDependency(material.transmissionTexture, "transmission", "Linear");
                addTextureDependency(material.specularTexture, "specular", "Linear");
                addTextureDependency(material.specularColorTexture, "specularColor", "sRGB");
                addTextureDependency(material.sheenColorTexture, "sheenColor", "sRGB");
                addTextureDependency(material.sheenRoughnessTexture, "sheenRoughness", "Linear");
                addTextureDependency(material.iridescenceTexture, "iridescence", "Linear");
                addTextureDependency(material.iridescenceThicknessTexture, "iridescenceThickness", "Linear");
                addTextureDependency(material.anisotropyTexture, "anisotropy", "Linear");
                const nlohmann::json pbrMetadata = materialPbrMetadataJson(material);
                const nlohmann::json materialPayload = sceneCacheSlicePayload("Material", i, materialGuid);
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
                    {"alphaMode", materialAlphaModeLabel(material.alphaMode)},
                    {"doubleSided", material.doubleSided != 0},
                    {"pbr", pbrMetadata},
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
                for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
                    if (primitive.material.valid() && primitive.material.index < materialGuids.size()) {
                        primitiveMaterials.push_back(materialGuids[primitive.material.index]);
                    }
                }
                const nlohmann::json meshPayload = sceneCacheSlicePayload("Mesh", i, meshGuid);
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
                record.sourcePath = effectiveSourceString;
                record.importedPath = genericRelativeOrValue(meshPath, workspace.root);
                record.cachePath = meshPayload.value("cachePath", genericRelativeOrValue(meshCache, workspace.root));
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

            setProgress(0.86f, "Writing prefab metadata");
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
                {"sourcePath", effectiveSourceString},
                {"originalSourcePath", originalSourceString},
                {"copiedSourcePath", copiedSourceString},
                {"runtimePayload", runtimePayload},
                {"rootNodes", scene.rootNodes},
                {"nodes", prefabNodes},
            };

            cache["runtimePayload"] = runtimePayload;
            cache["cookedPayloads"] = cookedPayloads;
            cache["textureCount"] = textures.size();
            cache["materialCount"] = materials.size();
            cache["meshCount"] = meshes.size();
            cache["nodeCount"] = scene.nodes.size();
            cache["skeletalAnimationMetadata"] = skeletalAnimationMetadata;
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
        placeholder["objMetadata"] = objMetadata;
        collisionLodMetadata = objMetadata.value("collisionLodMetadata", nlohmann::json::object());
        placeholder["collisionLodMetadata"] = collisionLodMetadata;
        placeholder["sourceExtension"] = sourceExtension;
        placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
        placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
        cache["runtimePayload"] = runtimePayload;
        cache["cookedPayloads"] = cookedPayloads;
        cache["objMetadata"] = objMetadata;
        cache["collisionLodMetadata"] = collisionLodMetadata;
    } else if (sourceIsMtl) {
        const auto inspectStart = std::chrono::steady_clock::now();
        setProgress(0.45f, "Inspecting MTL source");
        mtlMetadata = inspectMtlSource(effectiveSourcePath, result.warnings);
        result.workerInspectMs = elapsedMilliseconds(inspectStart);

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
        placeholder["mtlMetadata"] = mtlMetadata;
        placeholder["sourceExtension"] = sourceExtension;
        placeholder["sourceBytes"] = fileSizeOrZero(effectiveSourcePath);
        placeholder["originalSourceBytes"] = fileSizeOrZero(originalSourcePath);
        cache["runtimePayload"] = runtimePayload;
        cache["cookedPayloads"] = cookedPayloads;
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
        cookedPayloads.push_back(runtimePayload);

        placeholder["runtimePayload"] = runtimePayload;
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
                {"unitScale", request.settings.unitScale},
                {"coordinateConversion", request.settings.coordinateConversion},
            }},
        }},
        {"runtimePayload", runtimePayload.is_null() ? nlohmann::json::object() : runtimePayload},
        {"cookedPayloads", cookedPayloads},
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
    setProgress(1.0f, "Import staged");
    return result;
}

} // namespace rtv
