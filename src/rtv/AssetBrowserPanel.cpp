#include "rtv/AssetBrowserPanel.h"

#include "rtv/AssetImport.h"
#include "rtv/AssetManager.h"
#include "rtv/EditorPreferences.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/FileDialog.h"
#include "rtv/GpuScene.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <nlohmann/json.hpp>

#include <stb_image.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>
#endif

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rtv {

namespace {

template <size_t N>
void setTextBuffer(std::array<char, N>& buffer, const std::string& value) {
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::memcpy(buffer.data(), value.data(), std::min(value.size(), buffer.size() - 1));
}

template <size_t N>
void setPathBuffer(std::array<char, N>& buffer, const std::filesystem::path& path) {
    setTextBuffer(buffer, path.string());
}

std::filesystem::path canonicalForCompare(const std::filesystem::path& path);

std::string lowerString(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string trimString(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

std::string quoteCommandPath(const std::filesystem::path& path) {
    std::string value = path.string();
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

std::string readCommandOutput(const std::string& command) {
    std::string output;
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return output;
    }
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

struct GitStatusSnapshot {
    bool loaded = false;
    std::unordered_map<std::string, std::string> exactStatusByPath;
    std::vector<std::pair<std::string, std::string>> statusEntries;
};

std::unordered_map<std::string, GitStatusSnapshot>& gitStatusSnapshots() {
    static std::unordered_map<std::string, GitStatusSnapshot> snapshots;
    return snapshots;
}

void clearGitStatusSnapshots() {
    gitStatusSnapshots().clear();
}

std::string normalizeGitStatusPath(std::string value) {
    value = trimString(std::move(value));
    std::replace(value.begin(), value.end(), '\\', '/');
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return lowerString(std::move(value));
}

std::string sourceControlStatusFromPorcelainCode(std::string code) {
    code = trimString(std::move(code));
    if (code == "??") return "Untracked";
    if (code.find('U') != std::string::npos) return "Conflict";
    if (code.find('D') != std::string::npos) return "Deleted";
    if (code.find('A') != std::string::npos) return "Added";
    if (code.find('M') != std::string::npos) return "Modified";
    if (code.find('R') != std::string::npos) return "Renamed";
    if (code.find('C') != std::string::npos) return "Copied";
    return code.empty() ? "Clean" : "Changed";
}

const GitStatusSnapshot& gitStatusSnapshotForRoot(const std::filesystem::path& gitRoot) {
    const std::string rootKey = canonicalForCompare(gitRoot).string();
    GitStatusSnapshot& snapshot = gitStatusSnapshots()[rootKey];
    if (snapshot.loaded) {
        return snapshot;
    }
    snapshot.loaded = true;

#ifdef _WIN32
    constexpr const char* stderrRedirect = " 2>NUL";
#else
    constexpr const char* stderrRedirect = " 2>/dev/null";
#endif
    const std::string command = "git -C " + quoteCommandPath(gitRoot) + " status --porcelain=v1 --untracked-files=all" + stderrRedirect;
    const std::string output = readCommandOutput(command);
    std::stringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.size() < 4) {
            continue;
        }
        const std::string status = sourceControlStatusFromPorcelainCode(line.substr(0, 2));
        std::string path = line.substr(3);
        const size_t renameSeparator = path.find(" -> ");
        if (renameSeparator != std::string::npos) {
            path = path.substr(renameSeparator + 4);
        }
        const std::string relativeKey = normalizeGitStatusPath(std::move(path));
        if (relativeKey.empty()) {
            continue;
        }
        snapshot.exactStatusByPath.emplace(relativeKey, status);
        snapshot.statusEntries.emplace_back(relativeKey, status);
    }
    return snapshot;
}

std::string lookupGitStatusSnapshot(const GitStatusSnapshot& snapshot, const std::filesystem::path& relativePath) {
    const std::string relativeKey = normalizeGitStatusPath(relativePath.generic_string());
    if (relativeKey.empty()) {
        return "Unavailable";
    }
    const auto exactIt = snapshot.exactStatusByPath.find(relativeKey);
    if (exactIt != snapshot.exactStatusByPath.end()) {
        return exactIt->second;
    }

    const std::string relativePrefix = relativeKey + "/";
    for (const auto& [entryPath, status] : snapshot.statusEntries) {
        if (entryPath.rfind(relativePrefix, 0) == 0 || relativeKey.rfind(entryPath + "/", 0) == 0) {
            return status;
        }
    }
    return "Clean";
}

void setPreferenceSaveStatus(bool saved, std::string& status, std::string successMessage, std::string failureDetail) {
    status = saved ? std::move(successMessage) : "Preference save failed: " + std::move(failureDetail);
}

std::vector<std::string> parseTagList(const std::string& value) {
    std::vector<std::string> tags;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trimString(std::move(item));
        if (!item.empty()) {
            tags.push_back(std::move(item));
        }
    }
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

std::string joinTagList(const std::vector<std::string>& tags) {
    std::ostringstream out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << tags[i];
    }
    return out.str();
}

bool recordHasTagMatch(const AssetRecord& record, const std::string& filter) {
    const std::string lowerFilter = lowerString(trimString(filter));
    if (lowerFilter.empty()) {
        return true;
    }
    for (const std::string& tag : record.tags) {
        if (lowerString(tag).find(lowerFilter) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> collectRegistryTags(const AssetRegistry* registry) {
    std::vector<std::string> tags;
    if (registry == nullptr) {
        return tags;
    }
    for (const AssetRecord& record : registry->records()) {
        for (const std::string& tag : record.tags) {
            const std::string trimmed = trimString(tag);
            if (!trimmed.empty()) {
                tags.push_back(trimmed);
            }
        }
    }
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

std::vector<std::string> mergedTagSuggestions(std::vector<std::string> tags, const EditorPreferences* prefs) {
    if (prefs != nullptr) {
        tags.insert(tags.end(), prefs->assetTagPresets.begin(), prefs->assetTagPresets.end());
    }
    for (std::string& tag : tags) {
        tag = trimString(std::move(tag));
    }
    tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& tag) { return tag.empty(); }), tags.end());
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

bool tagListContains(const std::vector<std::string>& tags, const std::string& tag) {
    const std::string target = lowerString(trimString(tag));
    if (target.empty()) {
        return false;
    }
    return std::any_of(tags.begin(), tags.end(), [&](const std::string& value) {
        return lowerString(trimString(value)) == target;
    });
}

bool collectionContainsAsset(const EditorAssetCollection& collection, const AssetGuid& guid) {
    return std::find(collection.assetGuids.begin(), collection.assetGuids.end(), guid) != collection.assetGuids.end();
}

bool assetGuidListContains(const std::vector<std::string>& guids, const AssetGuid& guid) {
    return !guid.empty() && std::find(guids.begin(), guids.end(), guid) != guids.end();
}

bool preferencePathListContains(const std::vector<std::string>& paths, const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    const std::filesystem::path target = canonicalForCompare(path);
    return std::any_of(paths.begin(), paths.end(), [&](const std::string& value) {
        return !value.empty() && canonicalForCompare(std::filesystem::path(value)) == target;
    });
}

std::string matchingPreferencePathValue(const std::vector<std::string>& paths, const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    const std::filesystem::path target = canonicalForCompare(path);
    for (const std::string& value : paths) {
        if (!value.empty() && canonicalForCompare(std::filesystem::path(value)) == target) {
            return value;
        }
    }
    return {};
}

const EditorAssetCollection* selectedCollection(const EditorPreferences* prefs, int filterIndex) {
    if (prefs == nullptr || filterIndex <= 0) {
        return nullptr;
    }
    const size_t collectionIndex = static_cast<size_t>(filterIndex - 1);
    if (collectionIndex >= prefs->assetCollections.size()) {
        return nullptr;
    }
    return &prefs->assetCollections[collectionIndex];
}

bool isModelAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

bool isPlaceablePrefabSourcePath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".gltf" || ext == ".glb";
}

bool isTextureAssetPath(const std::filesystem::path& path) {
    return editorIsTexturePath(path);
}

bool isEnvironmentAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".hdr" || ext == ".exr";
}

bool isImportableSourceAssetPath(const std::filesystem::path& path) {
    return isModelAssetPath(path) || lowerString(path.extension().string()) == ".mtl" || isTextureAssetPath(path) || isEnvironmentAssetPath(path);
}

bool isRasterThumbnailPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr";
}

bool isSceneAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".rtlevel" || ext == ".mscene";
}

bool isProjectAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".vproject";
}

bool isMaterialAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".mtl";
}

bool isIesAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".ies";
}

bool isVolumeAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".vdb";
}

std::string contentKindLabel(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) {
        return "Folder";
    }
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".rtlevel") return "Scene";
    if (ext == ".mscene") return "Minitech Scene";
    if (ext == ".vproject") return "Project";
    if (ext == ".gltf" || ext == ".glb") return "Model";
    if (ext == ".obj") return "OBJ Model";
    if (ext == ".mtl") return "Material";
    if (isTextureAssetPath(path)) return "Texture";
    if (ext == ".hdr" || ext == ".exr") return "Environment";
    if (ext == ".ies") return "IES Profile";
    if (ext == ".vdb") return "Volume";
    if (ext == ".glsl" || ext == ".hlsl" || ext == ".spv") return "Shader";
    if (ext == ".json") return "JSON";
    return ext.empty() ? "File" : ext.substr(1);
}

std::string compactPathLabel(const std::filesystem::path& path, const char* fallback) {
    if (!path.empty()) {
        const std::filesystem::path name = path.filename();
        if (!name.empty()) {
            return name.string();
        }
        const std::filesystem::path root = path.root_name();
        if (!root.empty()) {
            return root.string();
        }
    }
    return fallback != nullptr ? fallback : "";
}

bool supportedContentPath(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) return true;
    const std::string ext = lowerString(path.extension().string());
    return ext == ".rtlevel" || ext == ".mscene" || ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".mtl" ||
        ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr" || ext == ".exr" ||
        ext == ".ies" || ext == ".vdb" || ext == ".vproject";
}

bool canOpenOrApplyPath(const std::filesystem::path& path) {
    return isSceneAssetPath(path) || isModelAssetPath(path) || isEnvironmentAssetPath(path);
}

void copyPathToClipboard(const std::filesystem::path& path) {
    const std::string text = path.string();
    ImGui::SetClipboardText(text.c_str());
}

void revealPathInFileBrowser(const std::filesystem::path& path) {
#ifdef _WIN32
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path target = ec ? path : absolute;
    if (std::filesystem::is_directory(target, ec)) {
        const std::string directory = target.string();
        ShellExecuteA(nullptr, "open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        const std::string args = "/select,\"" + target.string() + "\"";
        ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
#else
    (void)path;
#endif
}

std::filesystem::path canonicalForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return std::filesystem::absolute(path, ec);
}

bool pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (path.empty() || root.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(canonicalForCompare(path), canonicalForCompare(root), ec);
    if (ec) {
        return false;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> findGitRoot(std::filesystem::path start) {
    if (start.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(start, ec)) {
        start = start.parent_path();
    }
    start = canonicalForCompare(start);
    while (!start.empty()) {
        if (std::filesystem::exists(start / ".git", ec)) {
            return start;
        }
        const std::filesystem::path parent = start.parent_path();
        if (parent == start || parent.empty()) {
            break;
        }
        start = parent;
    }
    return std::nullopt;
}

std::string gitStatusLabelForPath(const std::filesystem::path& workspaceRoot, const std::filesystem::path& path) {
    if (path.empty()) {
        return "Unavailable";
    }
    std::optional<std::filesystem::path> gitRoot = findGitRoot(path);
    if (!gitRoot.has_value() && !workspaceRoot.empty()) {
        gitRoot = findGitRoot(workspaceRoot);
    }
    if (!gitRoot.has_value()) {
        return "Not in Git";
    }
    if (!pathIsWithin(path, *gitRoot)) {
        return "External";
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(canonicalForCompare(path), *gitRoot, ec);
    if (ec) {
        return "Unavailable";
    }
    return lookupGitStatusSnapshot(gitStatusSnapshotForRoot(*gitRoot), relative);
}

float assetImportProgress(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Imported && !record.missing && !record.stale) {
        return 1.0f;
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return 0.65f;
    }
    if (record.status == AssetImportStatus::Missing || record.missing) {
        return 0.20f;
    }
    if (record.status == AssetImportStatus::Failed) {
        return 0.0f;
    }
    return 0.35f;
}

const char* assetImportProgressLabel(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Imported && !record.missing && !record.stale) {
        return record.sourceMissing ? "Ready from cooked payload" : "Ready";
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return "Needs reimport";
    }
    if (record.status == AssetImportStatus::Missing || record.missing) {
        if (record.cookedPayloadMissing) return "Cooked payload missing";
        if (record.importedMetadataMissing) return "Metadata missing";
        if (record.dependenciesMissing) return "Dependency missing";
        return "Broken reference";
    }
    if (record.status == AssetImportStatus::Failed) {
        return "Failed";
    }
    return "Pending metadata";
}

const char* selectedAssetStateLabel(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Failed) {
        return "Failed";
    }
    if (record.status == AssetImportStatus::Missing || record.missing) {
        if (record.cookedPayloadMissing) return "Cooked payload missing";
        if (record.importedMetadataMissing) return "Metadata missing";
        if (record.dependenciesMissing) return "Dependency missing";
        return "Broken reference";
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return "Stale / needs reimport";
    }
    if (record.status == AssetImportStatus::Imported) {
        return record.sourceMissing ? "Ready from cooked payload" : "Ready";
    }
    return "Pending metadata";
}

ImVec4 selectedAssetStateColor(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Failed || record.status == AssetImportStatus::Missing || record.missing) {
        return ImVec4(0.95f, 0.36f, 0.32f, 1.0f);
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return ImVec4(0.95f, 0.68f, 0.28f, 1.0f);
    }
    if (record.status == AssetImportStatus::Imported) {
        return ImVec4(0.54f, 0.82f, 0.60f, 1.0f);
    }
    return ImVec4(0.65f, 0.70f, 0.78f, 1.0f);
}

const char* selectedPathOriginLabel(const EditorRuntimeState& state, const std::filesystem::path& path) {
    if (state.project != nullptr) {
        if (pathIsWithin(path, state.project->contentRoot)) {
            return "Project content";
        }
        if (pathIsWithin(path, state.project->projectRoot / "SourceAssets")) {
            return "Project source asset";
        }
    }
    return "External or workspace file";
}

ImU32 contentIconColor(const std::filesystem::path& path) {
    const EditorGlyphIcon icon = editorGlyphForPath(path);
    switch (icon) {
    case EditorGlyphIcon::Folder:
        return IM_COL32(188, 194, 204, 255);
    case EditorGlyphIcon::Texture:
    case EditorGlyphIcon::Environment:
        return IM_COL32(184, 196, 211, 255);
    case EditorGlyphIcon::Model:
    case EditorGlyphIcon::SceneFile:
        return IM_COL32(188, 199, 216, 255);
    case EditorGlyphIcon::Material:
        return IM_COL32(198, 190, 212, 255);
    case EditorGlyphIcon::IesProfile:
        return IM_COL32(210, 198, 168, 255);
    case EditorGlyphIcon::VolumeFile:
        return IM_COL32(178, 204, 198, 255);
    case EditorGlyphIcon::ShaderFile:
    case EditorGlyphIcon::ConfigFile:
        return IM_COL32(178, 188, 204, 255);
    default:
        return IM_COL32(158, 166, 178, 255);
    }
}

EditorGlyphIcon editorGlyphForAssetType(AssetType type) {
    switch (type) {
    case AssetType::Mesh: return EditorGlyphIcon::Model;
    case AssetType::Material: return EditorGlyphIcon::Material;
    case AssetType::Texture: return EditorGlyphIcon::Texture;
    case AssetType::HDRI: return EditorGlyphIcon::Environment;
    case AssetType::Scene: return EditorGlyphIcon::SceneFile;
    case AssetType::Prefab: return EditorGlyphIcon::Model;
    case AssetType::Unknown:
    default: return EditorGlyphIcon::File;
    }
}

ImU32 assetTypeIconColor(AssetType type) {
    switch (type) {
    case AssetType::Mesh:
    case AssetType::Prefab:
    case AssetType::Scene:
        return IM_COL32(188, 199, 216, 255);
    case AssetType::Material:
        return IM_COL32(198, 190, 212, 255);
    case AssetType::Texture:
    case AssetType::HDRI:
        return IM_COL32(184, 196, 211, 255);
    case AssetType::Unknown:
    default:
        return IM_COL32(158, 166, 178, 255);
    }
}

void drawAssetTypeGlyph(AssetType type, ImVec2 min, ImVec2 max) {
    editorDrawIconGlyph(editorGlyphForAssetType(type), min, max, assetTypeIconColor(type));
}

void drawContentGlyph(const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    editorDrawIconGlyph(editorGlyphForPath(path), min, max, contentIconColor(path));
}

bool contentActionButton(const char* id, EditorGlyphIcon icon, const char* label, const char* tooltip) {
    const bool clicked = editorIconTextButton(id, icon, label);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

uint8_t toneMapHdrChannel(float value) {
    const float mapped = value <= 0.0f ? 0.0f : value / (1.0f + value);
    return static_cast<uint8_t>(std::clamp(std::pow(mapped, 1.0f / 2.2f), 0.0f, 1.0f) * 255.0f);
}

std::filesystem::path resolveAssetRecordPath(const EditorRuntimeState& state, const std::string& value) {
    if (value.empty()) {
        return {};
    }
    std::filesystem::path path(value);
    if (path.is_absolute()) {
        return path;
    }
    if (state.project != nullptr) {
        return state.project->projectRoot / path;
    }
    if (state.assetRegistry != nullptr && !state.assetRegistry->state().path.empty()) {
        return state.assetRegistry->state().path.parent_path() / path;
    }
    return path;
}

std::filesystem::path firstResolvedExistingRecordPath(const EditorRuntimeState& state, std::initializer_list<std::string> values) {
    std::filesystem::path firstResolved;
    for (const std::string& value : values) {
        const std::filesystem::path resolved = resolveAssetRecordPath(state, value);
        if (resolved.empty()) {
            continue;
        }
        if (firstResolved.empty()) {
            firstResolved = resolved;
        }
        std::error_code ec;
        if (std::filesystem::exists(resolved, ec)) {
            return resolved;
        }
    }
    return firstResolved;
}

std::filesystem::path recordPreviewPath(const EditorRuntimeState& state, const AssetRecord& record) {
    if (record.type == AssetType::Texture || record.type == AssetType::HDRI) {
        return firstResolvedExistingRecordPath(state, {record.thumbnailPath, record.cachePath, record.sourcePath, record.importedPath});
    }
    return firstResolvedExistingRecordPath(state, {record.thumbnailPath, record.sourcePath, record.importedPath, record.cachePath});
}

std::string fileSizeLabel(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::is_regular_file(path, ec) ? std::filesystem::file_size(path, ec) : 0;
    if (ec) {
        return "Size unavailable";
    }
    const double bytes = static_cast<double>(size);
    std::ostringstream out;
    if (bytes >= 1024.0 * 1024.0) {
        out << "Size " << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024.0) {
        out << "Size " << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB";
    } else {
        out << "Size " << size << " B";
    }
    return out.str();
}

std::optional<nlohmann::json> readJsonFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    try {
        nlohmann::json json;
        file >> json;
        return json;
    } catch (...) {
        return std::nullopt;
    }
}

size_t jsonArraySize(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json[key].is_array()) {
        return 0;
    }
    return json[key].size();
}

std::string countLabel(const char* label, size_t value) {
    return std::string(label) + " " + std::to_string(value);
}

bool samePathForOperation(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return false;
    }
    return canonicalForCompare(lhs) == canonicalForCompare(rhs);
}

bool sceneLoadStatusIsSuccessfulCompletion(const std::string& status) {
    const std::string lower = lowerString(status);
    return lower.find("completed") != std::string::npos &&
        lower.find("failed") == std::string::npos &&
        lower.find("cancelled") == std::string::npos &&
        lower.find("error") == std::string::npos;
}

bool textureAssetMatchesPath(const TextureAsset& texture, const std::filesystem::path& path) {
    if (path.empty() || texture.sourcePath.empty()) {
        return false;
    }
    if (canonicalForCompare(texture.sourcePath) == canonicalForCompare(path)) {
        return true;
    }
    return lowerString(texture.sourcePath.filename().string()) == lowerString(path.filename().string());
}

std::optional<uint32_t> materialTextureSlotForPath(const EditorRuntimeState& state, const std::filesystem::path& path) {
    if (state.importedScene == nullptr || state.assets == nullptr || !isTextureAssetPath(path)) {
        return std::nullopt;
    }
    const SceneAsset& scene = *state.importedScene;
    for (uint32_t slot = 0; slot < scene.textures.size(); ++slot) {
        const TextureAsset* texture = state.assets->texture(scene.textures[slot]);
        if (texture != nullptr && textureAssetMatchesPath(*texture, path)) {
            return slot;
        }
    }
    return std::nullopt;
}

uint64_t fnv1a64(const std::string& text) {
    uint64_t value = 14695981039346656037ull;
    for (unsigned char ch : text) {
        value ^= static_cast<uint64_t>(ch);
        value *= 1099511628211ull;
    }
    return value;
}

std::string hex64(uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

int64_t pathWriteStamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    return stamp.time_since_epoch().count();
}

uintmax_t pathSizeForCache(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return 0;
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

bool isGeneratedPreviewDiskCacheCandidate(const std::filesystem::path& path) {
    std::error_code ec;
    return isModelAssetPath(path) || isSceneAssetPath(path) || isProjectAssetPath(path) || isMaterialAssetPath(path) ||
        isIesAssetPath(path) || isVolumeAssetPath(path) || std::filesystem::is_directory(path, ec);
}

std::optional<uint32_t> loadedMaterialIndexForRecord(const EditorRuntimeState& state, const AssetRecord& record) {
    if (state.importedScene == nullptr || record.type != AssetType::Material || record.sourceHash.empty() || record.importSettingsHash.empty()) {
        return std::nullopt;
    }
    const auto& materials = state.importedScene->materials;
    for (size_t i = 0; i < materials.size(); ++i) {
        if (importedAssetGuidFor(record.sourceHash, record.importSettingsHash, "Material", i) == record.guid && materials[i].valid()) {
            return materials[i].index;
        }
    }
    return std::nullopt;
}

struct AssetUsageSummary {
    size_t registryReferences = 0;
    size_t sceneReferences = 0;

    [[nodiscard]] bool referenced() const {
        return registryReferences > 0 || sceneReferences > 0;
    }
};

AssetUsageSummary assetUsageSummaryForRecord(const EditorRuntimeState& state, const AssetRecord& record) {
    AssetUsageSummary summary;
    if (state.assetRegistry != nullptr) {
        for (const AssetRecord& candidate : state.assetRegistry->records()) {
            if (candidate.guid == record.guid) {
                continue;
            }
            for (const AssetDependency& dependency : candidate.dependencies) {
                if (dependency.guid == record.guid) {
                    ++summary.registryReferences;
                }
            }
            for (const AssetGuid& reference : candidate.references) {
                if (reference == record.guid) {
                    ++summary.registryReferences;
                }
            }
        }
    }
    if (state.sceneDocument != nullptr) {
        for (const Entity* entity : state.sceneDocument->registry().entities()) {
            if (entity == nullptr || !entity->meshRenderer.has_value()) {
                continue;
            }
            const MeshRenderer& renderer = *entity->meshRenderer;
            if (renderer.meshGuid == record.guid) {
                ++summary.sceneReferences;
            }
            for (const MaterialSlot& slot : renderer.materialSlots) {
                if (slot.materialGuid == record.guid) {
                    ++summary.sceneReferences;
                }
                if (slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == record.guid) {
                    ++summary.sceneReferences;
                }
            }
        }
        for (const PrefabInstance& instance : state.sceneDocument->prefabInstances()) {
            if (instance.prefabGuid == record.guid) {
                ++summary.sceneReferences;
            }
        }
    }
    return summary;
}

bool regularFileExists(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::filesystem::path assetValidationReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot) {
    if (state.project != nullptr && !state.project->savedRoot.empty()) {
        return state.project->savedRoot / "Reports" / "asset_validation_report.json";
    }
    if (state.assetRegistry != nullptr && !state.assetRegistry->state().path.empty()) {
        return state.assetRegistry->state().path.parent_path() / "Reports" / "asset_validation_report.json";
    }
    return browserRoot / "Saved" / "Reports" / "asset_validation_report.json";
}

std::string safeReportName(std::string value) {
    if (value.empty()) {
        return "asset";
    }
    for (char& c : value) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || std::isspace(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    return value;
}

std::filesystem::path selectedAssetValidationReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_validation_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetRelationshipReportPath(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& guid,
    const char* relationshipKind) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename(std::string("asset_") + relationshipKind + "_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetDeleteReadinessReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_delete_readiness_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetProjectReferenceReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_project_references_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetBrokenPlaceholderReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_broken_placeholder_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetOverwriteRiskReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_overwrite_risk_" + safeReportName(guid) + ".json");
    return path;
}

struct AssetOverwriteRisk {
    std::string label;
    std::filesystem::path path;
    std::string status;
};

std::filesystem::path sourceControlDiffReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const std::filesystem::path& path) {
    std::filesystem::path reportPath = assetValidationReportPath(state, browserRoot);
    const std::string name = path.filename().empty() ? std::string("path") : path.filename().string();
    const std::string key = canonicalForCompare(path).string();
    reportPath.replace_filename("source_control_diff_" + safeReportName(name) + "_" + hex64(fnv1a64(key)) + ".patch");
    return reportPath;
}

bool sourceControlDiffReportAvailable(const std::string& status) {
    return status == "Modified" || status == "Added" || status == "Deleted" || status == "Renamed" || status == "Copied" ||
        status == "Conflict" || status == "Changed" || status == "Untracked";
}

bool sourceControlOverwriteRiskStatus(const std::string& status) {
    return sourceControlDiffReportAvailable(status);
}

std::vector<AssetOverwriteRisk> collectAssetOverwriteRisks(
    const EditorRuntimeState& state,
    const AssetRecord& record,
    const std::function<std::string(const std::filesystem::path&)>& statusForPath) {
    std::vector<AssetOverwriteRisk> risks;
    std::unordered_set<std::string> seen;
    auto addPath = [&](std::string label, const std::string& value) {
        const std::filesystem::path path = resolveAssetRecordPath(state, value);
        if (path.empty()) {
            return;
        }
        const std::string key = canonicalForCompare(path).string();
        if (!seen.insert(key).second) {
            return;
        }
        const std::string status = statusForPath(path);
        if (sourceControlOverwriteRiskStatus(status)) {
            risks.push_back(AssetOverwriteRisk{std::move(label), path, status});
        }
    };

    addPath("Imported metadata", record.importedPath);
    addPath("Cooked/runtime payload", record.cachePath);
    addPath("Thumbnail", record.thumbnailPath);
    return risks;
}

ImVec4 sourceControlStatusTextColor(const std::string& status) {
    if (status == "Clean") return ImVec4(0.54f, 0.82f, 0.60f, 1.0f);
    if (status == "Modified" || status == "Added" || status == "Renamed" || status == "Copied") return ImVec4(0.95f, 0.68f, 0.28f, 1.0f);
    if (status == "Deleted" || status == "Conflict") return ImVec4(0.95f, 0.36f, 0.32f, 1.0f);
    if (status == "Untracked") return ImVec4(0.55f, 0.72f, 0.95f, 1.0f);
    return ImVec4(0.65f, 0.70f, 0.78f, 1.0f);
}

bool writeSourceControlDiffReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& path,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (path.empty()) {
        outError = "No source-control path selected.";
        return false;
    }
    std::optional<std::filesystem::path> gitRoot = findGitRoot(path);
    if (!gitRoot.has_value() && !workspaceRoot.empty()) {
        gitRoot = findGitRoot(workspaceRoot);
    }
    if (!gitRoot.has_value()) {
        outError = "Path is not inside a Git repository.";
        return false;
    }
    if (!pathIsWithin(path, *gitRoot)) {
        outError = "Path is outside the resolved Git repository.";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(canonicalForCompare(path), *gitRoot, ec);
    if (ec) {
        outError = "Could not resolve repository-relative path: " + ec.message();
        return false;
    }

#ifdef _WIN32
    constexpr const char* stderrRedirect = " 2>NUL";
#else
    constexpr const char* stderrRedirect = " 2>/dev/null";
#endif
    const std::string rootArg = quoteCommandPath(*gitRoot);
    const std::string pathArg = quoteCommandPath(relative);
    const std::string statusText = readCommandOutput("git -C " + rootArg + " status --short -- " + pathArg + stderrRedirect);
    const std::string unstagedDiff = readCommandOutput("git -C " + rootArg + " diff -- " + pathArg + stderrRedirect);
    const std::string stagedDiff = readCommandOutput("git -C " + rootArg + " diff --cached -- " + pathArg + stderrRedirect);

    outPath = sourceControlDiffReportPath(state, browserRoot, path);
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create source-control report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write source-control diff report: " + outPath.string();
        return false;
    }

    file << "# Source Control Diff Report\n";
    file << "Repository: " << gitRoot->generic_string() << "\n";
    file << "Path: " << relative.generic_string() << "\n";
    file << "Status:\n" << (trimString(statusText).empty() ? std::string("  Clean\n") : statusText) << "\n";
    file << "## Unstaged Diff\n";
    file << (unstagedDiff.empty() ? std::string("(none)\n") : unstagedDiff);
    file << "\n## Staged Diff\n";
    file << (stagedDiff.empty() ? std::string("(none)\n") : stagedDiff);
    if (unstagedDiff.empty() && stagedDiff.empty() && trimString(statusText).rfind("??", 0) == 0) {
        file << "\n## Note\nUntracked files have no Git diff until they are added to the index.\n";
    }
    return true;
}

nlohmann::json assetRecordSummaryJson(const EditorRuntimeState& state, const AssetRecord& record) {
    const std::filesystem::path sourcePath = resolveAssetRecordPath(state, record.sourcePath);
    const std::filesystem::path importedPath = resolveAssetRecordPath(state, record.importedPath);
    const std::filesystem::path cachePath = resolveAssetRecordPath(state, record.cachePath);
    const std::filesystem::path thumbnailPath = resolveAssetRecordPath(state, record.thumbnailPath);
    return {
        {"guid", record.guid},
        {"displayName", record.displayName},
        {"assetType", assetTypeName(record.type)},
        {"status", assetImportStatusName(record.status)},
        {"sourcePath", record.sourcePath},
        {"importedPath", record.importedPath},
        {"cachePath", record.cachePath},
        {"thumbnailPath", record.thumbnailPath},
        {"tags", record.tags},
        {"resolvedSourcePath", sourcePath.empty() ? std::string{} : sourcePath.generic_string()},
        {"resolvedImportedPath", importedPath.empty() ? std::string{} : importedPath.generic_string()},
        {"resolvedCachePath", cachePath.empty() ? std::string{} : cachePath.generic_string()},
        {"resolvedThumbnailPath", thumbnailPath.empty() ? std::string{} : thumbnailPath.generic_string()},
        {"sourceMissing", record.sourceMissing},
        {"importedMetadataMissing", record.importedMetadataMissing},
        {"cookedPayloadMissing", record.cookedPayloadMissing},
        {"dependenciesMissing", record.dependenciesMissing},
        {"stale", record.stale},
        {"missing", record.missing},
    };
}

bool writeAssetOverwriteRiskReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetRecord& record,
    const std::vector<AssetOverwriteRisk>& risks,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetOverwriteRiskReportPath(state, browserRoot, record.guid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create overwrite-risk report folder: " + ec.message();
        return false;
    }

    nlohmann::json riskArray = nlohmann::json::array();
    for (const AssetOverwriteRisk& risk : risks) {
        riskArray.push_back({
            {"label", risk.label},
            {"path", risk.path.empty() ? std::string{} : risk.path.generic_string()},
            {"sourceControlStatus", risk.status},
            {"recommendedAction", "Review the diff or commit/stash the external change before reimporting or rebuilding payloads."},
        });
    }

    const nlohmann::json report = {
        {"schema", "TransparentAssetOverwriteRiskReportV1"},
        {"asset", assetRecordSummaryJson(state, record)},
        {"riskCount", risks.size()},
        {"overwriteRisks", riskArray},
        {"policy", {
            {"warning", "Reimport and Rebuild Payload may overwrite generated asset metadata, cooked payloads, or thumbnails that have external source-control changes."},
            {"affectedActions", {"Reimport", "Rebuild Payload"}},
            {"reloadPolicy", "This report does not reload changed files. Refresh/reimport remains an explicit user action."},
        }},
    };

    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write overwrite-risk report: " + outPath.string();
        return false;
    }
    file << report.dump(2);
    return true;
}

const AssetRecord* findAssetRecordByGuid(const AssetRegistry& registry, const AssetGuid& guid) {
    for (const AssetRecord& record : registry.records()) {
        if (record.guid == guid) {
            return &record;
        }
    }
    return nullptr;
}

void appendValidationIssue(
    nlohmann::json& array,
    const char* severity,
    const char* kind,
    const AssetRecord& record,
    std::string detail,
    std::string path = {}) {
    array.push_back({
        {"severity", severity},
        {"kind", kind},
        {"guid", record.guid},
        {"displayName", record.displayName},
        {"assetType", assetTypeName(record.type)},
        {"detail", std::move(detail)},
        {"path", std::move(path)},
    });
}

void appendComponentReferenceIssue(
    nlohmann::json& array,
    const Entity& entity,
    const char* component,
    const char* field,
    const AssetGuid& guid) {
    array.push_back({
        {"severity", "error"},
        {"kind", "InvalidComponentReference"},
        {"entity", entity.name},
        {"entityUuid", entity.uuid},
        {"component", component},
        {"field", field},
        {"guid", guid},
        {"detail", "Component references an asset GUID that is not present in the asset registry."},
    });
}

bool supportedCoordinateConversion(std::string_view value) {
    return value == "None" || value == "glTF Y-Up to Engine" || value == "Z-Up to Engine";
}

bool projectReferenceScanFileCandidate(const std::filesystem::path& path) {
    const std::string filename = lowerString(path.filename().string());
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".rtlevel" || ext == ".mscene" || ext == ".vproject") {
        return true;
    }
    auto endsWith = [&](const char* suffix) {
        const std::string value(suffix);
        return filename.size() >= value.size() && filename.compare(filename.size() - value.size(), value.size(), value) == 0;
    };
    return endsWith(".rtprefab.json") ||
        endsWith(".rtmesh.json") ||
        endsWith(".rtmaterial.json") ||
        endsWith(".rttexture.json") ||
        endsWith(".rthdri.json");
}

void appendUniqueScanRoot(std::vector<std::filesystem::path>& roots, const std::filesystem::path& root) {
    if (root.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return;
    }
    const std::filesystem::path canonical = canonicalForCompare(root);
    for (const std::filesystem::path& existing : roots) {
        if (canonicalForCompare(existing) == canonical) {
            return;
        }
    }
    roots.push_back(canonical);
}

std::string jsonPathChild(std::string parent, const std::string& child) {
    if (parent.empty()) {
        parent = "$";
    }
    return parent + "/" + child;
}

void appendGuidOccurrences(const nlohmann::json& value, const AssetGuid& targetGuid, const std::string& jsonPath, nlohmann::json& occurrences) {
    if (value.is_string()) {
        if (value.get<std::string>() == targetGuid) {
            occurrences.push_back({{"jsonPath", jsonPath.empty() ? "$" : jsonPath}});
        }
        return;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            appendGuidOccurrences(it.value(), targetGuid, jsonPathChild(jsonPath, it.key()), occurrences);
        }
        return;
    }
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            appendGuidOccurrences(value[i], targetGuid, jsonPathChild(jsonPath, std::to_string(i)), occurrences);
        }
    }
}

void collectProjectReferenceScanFiles(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    nlohmann::json& checkedRoots,
    std::vector<std::filesystem::path>& files) {
    std::vector<std::filesystem::path> roots;
    if (state.project != nullptr) {
        appendUniqueScanRoot(roots, state.project->contentRoot);
        appendUniqueScanRoot(roots, state.project->scenesRoot);
    } else {
        appendUniqueScanRoot(roots, browserRoot);
    }

    for (const std::filesystem::path& root : roots) {
        checkedRoots.push_back(root.generic_string());
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            std::error_code entryError;
            if (entry.is_regular_file(entryError) && projectReferenceScanFileCandidate(entry.path())) {
                files.push_back(canonicalForCompare(entry.path()));
            }
        }
    }
    if (state.project != nullptr && !state.project->projectFile.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(state.project->projectFile, ec) && projectReferenceScanFileCandidate(state.project->projectFile)) {
            files.push_back(canonicalForCompare(state.project->projectFile));
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
}

void appendInvalidSavedGuidReferences(
    const nlohmann::json& value,
    const std::unordered_set<AssetGuid>& registryGuids,
    const std::filesystem::path& filePath,
    const std::string& jsonPath,
    std::string objectKey,
    nlohmann::json& invalidReferences) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            appendInvalidSavedGuidReferences(it.value(), registryGuids, filePath, jsonPathChild(jsonPath, it.key()), it.key(), invalidReferences);
        }
        return;
    }
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            appendInvalidSavedGuidReferences(value[i], registryGuids, filePath, jsonPathChild(jsonPath, std::to_string(i)), objectKey, invalidReferences);
        }
        return;
    }
    if (!value.is_string()) {
        return;
    }

    const std::string keyLower = lowerString(std::move(objectKey));
    if (keyLower.find("guid") == std::string::npos) {
        return;
    }
    const std::string guid = value.get<std::string>();
    if (guid.empty() || registryGuids.find(guid) != registryGuids.end()) {
        return;
    }
    invalidReferences.push_back({
        {"severity", "error"},
        {"kind", "InvalidSavedProjectReference"},
        {"file", filePath.generic_string()},
        {"jsonPath", jsonPath.empty() ? "$" : jsonPath},
        {"field", keyLower},
        {"guid", guid},
        {"detail", "Saved project metadata contains a GUID field whose value is not present in the loaded asset registry."},
    });
}

nlohmann::json sourceControlPolicyReportJson(size_t copiedSourceAssetCount) {
    return {
        {"schema", "TransparentAssetMetadataV1"},
        {"commitImportedMetadata", true},
        {"commitCopiedSourceAssets", copiedSourceAssetCount > 0},
        {"copiedSourceAssetCount", copiedSourceAssetCount},
        {"commitCookedPayloads", false},
        {"commitThumbnails", false},
        {"regenerateCookedPayloadsWhenMissing", true},
        {"policy", "Commit deterministic Content metadata and copied SourceAssets when import settings internalize source files. Treat Cache payloads and thumbnails as generated unless a project-specific source-control policy says otherwise."},
    };
}

size_t countValidationSeverity(const std::vector<const nlohmann::json*>& arrays, std::string_view severity) {
    size_t count = 0;
    for (const nlohmann::json* array : arrays) {
        if (array == nullptr || !array->is_array()) {
            continue;
        }
        for (const nlohmann::json& item : *array) {
            if (item.is_object() && item.value("severity", std::string{}) == severity) {
                ++count;
            }
        }
    }
    return count;
}

nlohmann::json buildAssetValidationReport(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& targetGuid = {}) {
    const bool scopedToAsset = !targetGuid.empty();
    nlohmann::json missingSources = nlohmann::json::array();
    nlohmann::json missingImportedMetadata = nlohmann::json::array();
    nlohmann::json missingCookedPayloads = nlohmann::json::array();
    nlohmann::json missingDependencies = nlohmann::json::array();
    nlohmann::json staleAssets = nlohmann::json::array();
    nlohmann::json unsupportedImportSettings = nlohmann::json::array();
    nlohmann::json invalidComponentReferences = nlohmann::json::array();
    nlohmann::json invalidSavedProjectReferences = nlohmann::json::array();
    nlohmann::json savedProjectReferenceParseErrors = nlohmann::json::array();
    nlohmann::json savedProjectReferenceScanRoots = nlohmann::json::array();
    nlohmann::json requiresReimport = nlohmann::json::array();
    nlohmann::json missingThumbnails = nlohmann::json::array();
    nlohmann::json reverseAssetReferences = nlohmann::json::array();
    nlohmann::json currentSceneReferences = nlohmann::json::array();
    size_t copiedSourceAssetCount = 0;
    size_t validatedAssetCount = 0;
    nlohmann::json selectedAsset = nlohmann::json::object();

    std::unordered_set<AssetGuid> registryGuids;
    if (state.assetRegistry != nullptr) {
        registryGuids.reserve(state.assetRegistry->records().size());
        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (!record.guid.empty()) {
                registryGuids.insert(record.guid);
            }
        }

        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (record.importSettings.copySourceIntoProject) {
                ++copiedSourceAssetCount;
            }
            if (scopedToAsset && record.guid != targetGuid) {
                for (const AssetDependency& dependency : record.dependencies) {
                    if (dependency.guid == targetGuid) {
                        reverseAssetReferences.push_back({
                            {"ownerGuid", record.guid},
                            {"ownerDisplayName", record.displayName},
                            {"ownerAssetType", assetTypeName(record.type)},
                            {"role", dependency.kind.empty() ? "dependency" : dependency.kind},
                            {"source", "Dependency"},
                        });
                    }
                }
                for (const AssetGuid& reference : record.references) {
                    if (reference == targetGuid) {
                        reverseAssetReferences.push_back({
                            {"ownerGuid", record.guid},
                            {"ownerDisplayName", record.displayName},
                            {"ownerAssetType", assetTypeName(record.type)},
                            {"role", "reference"},
                            {"source", "Reference"},
                        });
                    }
                }
                continue;
            }
            ++validatedAssetCount;
            if (scopedToAsset) {
                selectedAsset = {
                    {"guid", record.guid},
                    {"displayName", record.displayName},
                    {"assetType", assetTypeName(record.type)},
                    {"status", assetImportStatusName(record.status)},
                    {"sourcePath", record.sourcePath},
                    {"importedPath", record.importedPath},
                    {"cachePath", record.cachePath},
                    {"thumbnailPath", record.thumbnailPath},
                    {"tags", record.tags},
                };
            }
            const std::filesystem::path sourcePath = resolveAssetRecordPath(state, record.sourcePath);
            const std::filesystem::path importedPath = resolveAssetRecordPath(state, record.importedPath);
            const std::filesystem::path cachePath = resolveAssetRecordPath(state, record.cachePath);
            const std::filesystem::path thumbnailPath = resolveAssetRecordPath(state, record.thumbnailPath);
            const bool sourceMissing = !record.sourcePath.empty() && !regularFileExists(sourcePath);
            const bool importedMissing = !record.importedPath.empty() && !regularFileExists(importedPath);
            const bool cookedMissing = !record.cachePath.empty() && !regularFileExists(cachePath);
            const bool thumbnailMissing = !record.thumbnailPath.empty() && !regularFileExists(thumbnailPath);

            if (sourceMissing || record.sourceMissing) {
                appendValidationIssue(missingSources, "warning", "MissingSource", record, "Raw import source is missing.", record.sourcePath);
            }
            if (importedMissing || record.importedMetadataMissing) {
                appendValidationIssue(missingImportedMetadata, "error", "MissingImportedMetadata", record, "Imported asset metadata file is missing.", record.importedPath);
            }
            if (cookedMissing || record.cookedPayloadMissing) {
                appendValidationIssue(missingCookedPayloads, "error", "MissingCookedPayload", record, "Cooked/runtime payload is missing.", record.cachePath);
            }
            if (thumbnailMissing) {
                appendValidationIssue(missingThumbnails, "warning", "MissingThumbnail", record, "Thumbnail preview path is missing; Content Browser will use a type fallback icon.", record.thumbnailPath);
            }
            for (const AssetDependency& dependency : record.dependencies) {
                if (!dependency.guid.empty() && registryGuids.find(dependency.guid) == registryGuids.end()) {
                    missingDependencies.push_back({
                        {"severity", "error"},
                        {"kind", "MissingDependencyGuid"},
                        {"ownerGuid", record.guid},
                        {"ownerDisplayName", record.displayName},
                        {"ownerAssetType", assetTypeName(record.type)},
                        {"dependencyGuid", dependency.guid},
                        {"dependencyKind", dependency.kind},
                        {"detail", "Dependency GUID is not present in the asset registry."},
                    });
                }
            }
            if (record.stale || record.status == AssetImportStatus::Stale) {
                appendValidationIssue(staleAssets, "warning", "StaleAsset", record, "Source is newer than imported metadata or cooked payload.");
            }
            if (record.status == AssetImportStatus::Failed || record.stale || importedMissing || cookedMissing) {
                appendValidationIssue(requiresReimport, record.status == AssetImportStatus::Failed ? "error" : "warning", "RequiresReimport", record, "Asset should be reimported or repaired before cooking/packaging.");
            }
            if (record.importSettings.unitScale <= 0.0f) {
                appendValidationIssue(unsupportedImportSettings, "error", "InvalidUnitScale", record, "Import unit scale must be greater than zero.");
            }
            if (!supportedCoordinateConversion(record.importSettings.coordinateConversion)) {
                appendValidationIssue(unsupportedImportSettings, "warning", "UnsupportedCoordinateConversion", record, "Import coordinate conversion is not recognized: " + record.importSettings.coordinateConversion);
            }
        }
    }

    if (state.sceneDocument != nullptr) {
        for (const Entity* entity : state.sceneDocument->registry().entities()) {
            if (entity == nullptr || !entity->meshRenderer.has_value()) {
                continue;
            }
            const MeshRenderer& renderer = *entity->meshRenderer;
            if (scopedToAsset && renderer.meshGuid == targetGuid) {
                currentSceneReferences.push_back({
                    {"entity", entity->name},
                    {"entityUuid", entity->uuid},
                    {"component", "MeshRenderer"},
                    {"field", "meshGuid"},
                    {"guid", renderer.meshGuid},
                });
            } else if (!scopedToAsset && !renderer.meshGuid.empty() && registryGuids.find(renderer.meshGuid) == registryGuids.end()) {
                appendComponentReferenceIssue(invalidComponentReferences, *entity, "MeshRenderer", "meshGuid", renderer.meshGuid);
            }
            for (const MaterialSlot& slot : renderer.materialSlots) {
                if (scopedToAsset && slot.materialGuid == targetGuid) {
                    currentSceneReferences.push_back({
                        {"entity", entity->name},
                        {"entityUuid", entity->uuid},
                        {"component", "MeshRenderer"},
                        {"field", "materialGuid"},
                        {"guid", slot.materialGuid},
                    });
                } else if (!scopedToAsset && !slot.materialGuid.empty() && registryGuids.find(slot.materialGuid) == registryGuids.end()) {
                    appendComponentReferenceIssue(invalidComponentReferences, *entity, "MeshRenderer", "materialGuid", slot.materialGuid);
                }
                if (scopedToAsset && slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == targetGuid) {
                    currentSceneReferences.push_back({
                        {"entity", entity->name},
                        {"entityUuid", entity->uuid},
                        {"component", "MeshRenderer"},
                        {"field", "overrideMaterialGuid"},
                        {"guid", *slot.overrideMaterialGuid},
                    });
                } else if (!scopedToAsset && slot.overrideMaterialGuid.has_value() && !slot.overrideMaterialGuid->empty() && registryGuids.find(*slot.overrideMaterialGuid) == registryGuids.end()) {
                    appendComponentReferenceIssue(invalidComponentReferences, *entity, "MeshRenderer", "overrideMaterialGuid", *slot.overrideMaterialGuid);
                }
            }
        }
        for (const PrefabInstance& instance : state.sceneDocument->prefabInstances()) {
            if (scopedToAsset && instance.prefabGuid == targetGuid) {
                currentSceneReferences.push_back({
                    {"entity", "Prefab Instance"},
                    {"entityUuid", instance.instanceRoot.index},
                    {"component", "PrefabInstance"},
                    {"field", "prefabGuid"},
                    {"guid", instance.prefabGuid},
                });
            } else if (!scopedToAsset && !instance.prefabGuid.empty() && registryGuids.find(instance.prefabGuid) == registryGuids.end()) {
                invalidComponentReferences.push_back({
                    {"severity", "error"},
                    {"kind", "InvalidPrefabInstanceReference"},
                    {"prefabGuid", instance.prefabGuid},
                    {"instanceRoot", instance.instanceRoot.index},
                    {"detail", "Prefab instance references an asset GUID that is not present in the asset registry."},
                });
            }
        }
    }

    size_t savedProjectReferenceScannedFileCount = 0;
    if (!scopedToAsset) {
        std::vector<std::filesystem::path> files;
        collectProjectReferenceScanFiles(state, browserRoot, savedProjectReferenceScanRoots, files);
        savedProjectReferenceScannedFileCount = files.size();
        for (const std::filesystem::path& path : files) {
            std::optional<nlohmann::json> json = readJsonFile(path);
            if (!json.has_value()) {
                savedProjectReferenceParseErrors.push_back({
                    {"severity", "warning"},
                    {"kind", "SavedProjectReferenceParseError"},
                    {"file", path.generic_string()},
                    {"detail", "File matched the project reference validation set but could not be parsed as JSON."},
                });
                continue;
            }
            appendInvalidSavedGuidReferences(*json, registryGuids, path, "$", {}, invalidSavedProjectReferences);
        }
    }

    const std::vector<const nlohmann::json*> issueArrays = {
        &missingSources,
        &missingImportedMetadata,
        &missingCookedPayloads,
        &missingThumbnails,
        &missingDependencies,
        &staleAssets,
        &unsupportedImportSettings,
        &invalidComponentReferences,
        &invalidSavedProjectReferences,
        &savedProjectReferenceParseErrors,
        &requiresReimport,
    };
    const size_t errorCount = countValidationSeverity(issueArrays, "error");
    const size_t warningCount = countValidationSeverity(issueArrays, "warning");
    return {
        {"version", 1},
        {"kind", scopedToAsset ? "SelectedAssetValidationReport" : "AssetValidationReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", selectedAsset},
        {"assetCount", scopedToAsset ? validatedAssetCount : state.assetRegistry != nullptr ? state.assetRegistry->records().size() : 0},
        {"errorCount", errorCount},
        {"warningCount", warningCount},
        {"sourceControlPolicy", sourceControlPolicyReportJson(copiedSourceAssetCount)},
        {"missingSources", missingSources},
        {"missingImportedMetadata", missingImportedMetadata},
        {"missingCookedPayloads", missingCookedPayloads},
        {"missingThumbnails", missingThumbnails},
        {"missingDependencies", missingDependencies},
        {"staleAssets", staleAssets},
        {"unsupportedImportSettings", unsupportedImportSettings},
        {"invalidComponentReferences", invalidComponentReferences},
        {"invalidSavedProjectReferences", invalidSavedProjectReferences},
        {"savedProjectReferenceParseErrors", savedProjectReferenceParseErrors},
        {"savedProjectReferenceScanRoots", savedProjectReferenceScanRoots},
        {"savedProjectReferenceScannedFileCount", savedProjectReferenceScannedFileCount},
        {"requiresReimport", requiresReimport},
        {"reverseAssetReferences", reverseAssetReferences},
        {"currentSceneReferences", currentSceneReferences},
    };
}

nlohmann::json buildAssetDependencyReport(const EditorRuntimeState& state, const AssetGuid& targetGuid) {
    nlohmann::json dependencies = nlohmann::json::array();
    nlohmann::json storedReferences = nlohmann::json::array();
    nlohmann::json selectedAsset = nlohmann::json::object();
    if (state.assetRegistry == nullptr) {
        return {
            {"version", 1},
            {"kind", "SelectedAssetDependencyReport"},
            {"targetGuid", targetGuid},
            {"selectedAsset", selectedAsset},
            {"dependencies", dependencies},
            {"storedReferences", storedReferences},
        };
    }

    const AssetRecord* target = findAssetRecordByGuid(*state.assetRegistry, targetGuid);
    if (target != nullptr) {
        selectedAsset = assetRecordSummaryJson(state, *target);
        for (const AssetDependency& dependency : target->dependencies) {
            const AssetRecord* linked = findAssetRecordByGuid(*state.assetRegistry, dependency.guid);
            dependencies.push_back({
                {"guid", dependency.guid},
                {"role", dependency.kind.empty() ? "dependency" : dependency.kind},
                {"found", linked != nullptr},
                {"asset", linked != nullptr ? assetRecordSummaryJson(state, *linked) : nlohmann::json::object()},
            });
        }
        for (const AssetGuid& reference : target->references) {
            const AssetRecord* linked = findAssetRecordByGuid(*state.assetRegistry, reference);
            storedReferences.push_back({
                {"guid", reference},
                {"role", "reference"},
                {"found", linked != nullptr},
                {"asset", linked != nullptr ? assetRecordSummaryJson(state, *linked) : nlohmann::json::object()},
            });
        }
    }

    return {
        {"version", 1},
        {"kind", "SelectedAssetDependencyReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", selectedAsset},
        {"dependencyCount", dependencies.size()},
        {"storedReferenceCount", storedReferences.size()},
        {"dependencies", dependencies},
        {"storedReferences", storedReferences},
    };
}

nlohmann::json buildAssetReferenceReport(const EditorRuntimeState& state, const AssetGuid& targetGuid) {
    nlohmann::json registryReferences = nlohmann::json::array();
    nlohmann::json currentSceneReferences = nlohmann::json::array();
    nlohmann::json selectedAsset = nlohmann::json::object();
    if (state.assetRegistry != nullptr) {
        const AssetRecord* target = findAssetRecordByGuid(*state.assetRegistry, targetGuid);
        if (target != nullptr) {
            selectedAsset = assetRecordSummaryJson(state, *target);
        }
        for (const AssetRecord& owner : state.assetRegistry->records()) {
            if (owner.guid == targetGuid) {
                continue;
            }
            for (const AssetDependency& dependency : owner.dependencies) {
                if (dependency.guid == targetGuid) {
                    registryReferences.push_back({
                        {"source", "Dependency"},
                        {"role", dependency.kind.empty() ? "dependency" : dependency.kind},
                        {"owner", assetRecordSummaryJson(state, owner)},
                    });
                }
            }
            for (const AssetGuid& reference : owner.references) {
                if (reference == targetGuid) {
                    registryReferences.push_back({
                        {"source", "Reference"},
                        {"role", "reference"},
                        {"owner", assetRecordSummaryJson(state, owner)},
                    });
                }
            }
        }
    }

    if (state.sceneDocument != nullptr) {
        for (const Entity* entity : state.sceneDocument->registry().entities()) {
            if (entity == nullptr || !entity->meshRenderer.has_value()) {
                continue;
            }
            const MeshRenderer& renderer = *entity->meshRenderer;
            auto appendSceneReference = [&](const char* field) {
                currentSceneReferences.push_back({
                    {"entity", entity->name},
                    {"entityUuid", entity->uuid},
                    {"component", "MeshRenderer"},
                    {"field", field},
                });
            };
            if (renderer.meshGuid == targetGuid) {
                appendSceneReference("meshGuid");
            }
            for (const MaterialSlot& slot : renderer.materialSlots) {
                if (slot.materialGuid == targetGuid) {
                    appendSceneReference("materialGuid");
                }
                if (slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == targetGuid) {
                    appendSceneReference("overrideMaterialGuid");
                }
            }
        }
        for (const PrefabInstance& instance : state.sceneDocument->prefabInstances()) {
            if (instance.prefabGuid == targetGuid) {
                currentSceneReferences.push_back({
                    {"entity", "Prefab Instance"},
                    {"entityUuid", instance.instanceRoot.index},
                    {"component", "PrefabInstance"},
                    {"field", "prefabGuid"},
                });
            }
        }
    }

    return {
        {"version", 1},
        {"kind", "SelectedAssetReferenceReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", selectedAsset},
        {"registryReferenceCount", registryReferences.size()},
        {"currentSceneReferenceCount", currentSceneReferences.size()},
        {"registryReferences", registryReferences},
        {"currentSceneReferences", currentSceneReferences},
    };
}

nlohmann::json buildAssetProjectReferenceScanReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    const std::unordered_set<std::string>& excludedFileKeys = {});

nlohmann::json buildAssetDeleteReadinessReport(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& targetGuid) {
    nlohmann::json referenceReport = buildAssetReferenceReport(state, targetGuid);
    std::unordered_set<std::string> excludedReferenceFiles;
    if (state.assetRegistry != nullptr) {
        if (const AssetRecord* target = findAssetRecordByGuid(*state.assetRegistry, targetGuid)) {
            const std::filesystem::path targetMetadataPath = resolveAssetRecordPath(state, target->importedPath);
            if (!targetMetadataPath.empty()) {
                excludedReferenceFiles.insert(canonicalForCompare(targetMetadataPath).string());
            }
        }
    }
    nlohmann::json projectReferenceScan = buildAssetProjectReferenceScanReport(state, browserRoot, targetGuid, excludedReferenceFiles);
    const size_t registryReferenceCount = referenceReport.value("registryReferenceCount", 0u);
    const size_t currentSceneReferenceCount = referenceReport.value("currentSceneReferenceCount", 0u);
    const size_t savedProjectReferenceCount = projectReferenceScan.value("referenceOccurrenceCount", 0u);
    const size_t savedProjectReferencingFileCount = projectReferenceScan.value("referencingFileCount", 0u);
    const size_t savedProjectParseErrorCount = projectReferenceScan.value("parseErrorCount", 0u);
    const bool blockedByCheckedData = registryReferenceCount > 0 || currentSceneReferenceCount > 0 || savedProjectReferenceCount > 0 || savedProjectParseErrorCount > 0;
    nlohmann::json blockers = nlohmann::json::array();

    if (registryReferenceCount > 0) {
        blockers.push_back({
            {"scope", "LoadedAssetRegistry"},
            {"severity", "warning"},
            {"count", registryReferenceCount},
            {"detail", "Loaded registry records still depend on or reference this asset GUID."},
        });
    }
    if (currentSceneReferenceCount > 0) {
        blockers.push_back({
            {"scope", "CurrentScene"},
            {"severity", "warning"},
            {"count", currentSceneReferenceCount},
            {"detail", "The current scene still contains component or prefab references to this asset GUID."},
        });
    }
    if (savedProjectReferenceCount > 0) {
        blockers.push_back({
            {"scope", "SavedProjectMetadata"},
            {"severity", "warning"},
            {"count", savedProjectReferenceCount},
            {"fileCount", savedProjectReferencingFileCount},
            {"detail", "Saved project metadata files contain this asset GUID."},
        });
    }
    if (savedProjectParseErrorCount > 0) {
        blockers.push_back({
            {"scope", "SavedProjectMetadata"},
            {"severity", "warning"},
            {"count", savedProjectParseErrorCount},
            {"detail", "One or more saved project metadata files could not be parsed, so saved-file readiness is not fully verified."},
        });
    }

    return {
        {"version", 1},
        {"kind", "SelectedAssetDeleteReadinessReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", referenceReport.value("selectedAsset", nlohmann::json::object())},
        {"deleteReadyForLoadedData", registryReferenceCount == 0 && currentSceneReferenceCount == 0},
        {"deleteReadyForSavedProjectFiles", savedProjectReferenceCount == 0 && savedProjectParseErrorCount == 0},
        {"deleteReadyForCheckedScopes", !blockedByCheckedData},
        {"registryReferenceCount", registryReferenceCount},
        {"currentSceneReferenceCount", currentSceneReferenceCount},
        {"savedProjectReferenceCount", savedProjectReferenceCount},
        {"savedProjectReferencingFileCount", savedProjectReferencingFileCount},
        {"savedProjectReferenceParseErrorCount", savedProjectParseErrorCount},
        {"blockers", blockers},
        {"registryReferences", referenceReport.value("registryReferences", nlohmann::json::array())},
        {"currentSceneReferences", referenceReport.value("currentSceneReferences", nlohmann::json::array())},
        {"savedProjectReferenceScan", projectReferenceScan},
        {"checkedScopes", nlohmann::json::array({"LoadedAssetRegistry", "CurrentScene", "SavedProjectMetadata"})},
        {"uncheckedScopes", nlohmann::json::array({"GeneratedCachePayloadInternals", "ExternalProjectFiles", "OpaquePackages"})},
        {"recommendation", blockedByCheckedData
            ? "Replace or remove loaded-registry, current-scene, and saved-project metadata references, and resolve saved metadata parse errors, before deleting this asset."
            : "No references were found in the loaded registry, current scene, or saved project metadata scan. Destructive deletion still remains disabled until cross-file rewrite and package/cache validation workflows are implemented."},
        {"destructiveActionEnabled", false},
        {"limitation", "This report checks loaded data plus saved project JSON metadata. It does not rewrite references or inspect generated cache payload internals, external project files, or opaque packages."},
    };
}

nlohmann::json buildAssetProjectReferenceScanReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    const std::unordered_set<std::string>& excludedFileKeys) {
    nlohmann::json selectedAsset = nlohmann::json::object();
    if (state.assetRegistry != nullptr) {
        if (const AssetRecord* target = findAssetRecordByGuid(*state.assetRegistry, targetGuid)) {
            selectedAsset = assetRecordSummaryJson(state, *target);
        }
    }

    nlohmann::json checkedRoots = nlohmann::json::array();
    std::vector<std::filesystem::path> files;
    collectProjectReferenceScanFiles(state, browserRoot, checkedRoots, files);

    nlohmann::json scannedFiles = nlohmann::json::array();
    nlohmann::json filesWithReferences = nlohmann::json::array();
    nlohmann::json excludedFilesWithReferences = nlohmann::json::array();
    nlohmann::json parseErrors = nlohmann::json::array();
    size_t occurrenceCount = 0;
    size_t excludedOccurrenceCount = 0;
    for (const std::filesystem::path& path : files) {
        scannedFiles.push_back(path.generic_string());
        std::optional<nlohmann::json> json = readJsonFile(path);
        if (!json.has_value()) {
            parseErrors.push_back({
                {"path", path.generic_string()},
                {"detail", "File matched the project reference scan set but could not be parsed as JSON."},
            });
            continue;
        }
        nlohmann::json occurrences = nlohmann::json::array();
        appendGuidOccurrences(*json, targetGuid, "$", occurrences);
        if (!occurrences.empty()) {
            const std::string fileKey = canonicalForCompare(path).string();
            if (excludedFileKeys.find(fileKey) != excludedFileKeys.end()) {
                excludedOccurrenceCount += occurrences.size();
                excludedFilesWithReferences.push_back({
                    {"path", path.generic_string()},
                    {"referenceCount", occurrences.size()},
                    {"occurrences", occurrences},
                    {"reason", "SelectedAssetMetadata"},
                });
                continue;
            }
            occurrenceCount += occurrences.size();
            filesWithReferences.push_back({
                {"path", path.generic_string()},
                {"referenceCount", occurrences.size()},
                {"occurrences", occurrences},
            });
        }
    }

    return {
        {"version", 1},
        {"kind", "SelectedAssetProjectReferenceScanReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", selectedAsset},
        {"checkedRoots", checkedRoots},
        {"scannedFileCount", scannedFiles.size()},
        {"referencingFileCount", filesWithReferences.size()},
        {"referenceOccurrenceCount", occurrenceCount},
        {"excludedReferenceOccurrenceCount", excludedOccurrenceCount},
        {"excludedFilesWithReferences", excludedFilesWithReferences},
        {"scannedFiles", scannedFiles},
        {"filesWithReferences", filesWithReferences},
        {"parseErrorCount", parseErrors.size()},
        {"parseErrors", parseErrors},
        {"checkedFileTypes", nlohmann::json::array({".rtlevel", ".mscene", ".vproject", ".rtprefab.json", ".rtmesh.json", ".rtmaterial.json", ".rttexture.json", ".rthdri.json"})},
        {"limitation", "This is a saved-file JSON scan for project content and scene roots. It does not rewrite references or inspect generated cache payload internals."},
    };
}

nlohmann::json buildAssetBrokenPlaceholderReport(const EditorRuntimeState& state, const AssetRecord& record) {
    const std::filesystem::path sourcePath = resolveAssetRecordPath(state, record.sourcePath);
    const std::filesystem::path importedPath = resolveAssetRecordPath(state, record.importedPath);
    const std::filesystem::path cachePath = resolveAssetRecordPath(state, record.cachePath);
    const bool sourceMissing = !record.sourcePath.empty() && !regularFileExists(sourcePath);
    const bool importedMissing = !record.importedPath.empty() && !regularFileExists(importedPath);
    const bool payloadMissing = !record.cachePath.empty() && !regularFileExists(cachePath);
    const bool broken = record.missing || record.status == AssetImportStatus::Missing || record.sourceMissing || record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing || sourceMissing || importedMissing || payloadMissing;

    nlohmann::json missingReasons = nlohmann::json::array();
    auto appendReason = [&](const char* kind, const char* severity, const char* detail, const std::string& storedPath, const std::filesystem::path& resolvedPath) {
        missingReasons.push_back({
            {"kind", kind},
            {"severity", severity},
            {"detail", detail},
            {"path", storedPath},
            {"resolvedPath", resolvedPath.empty() ? std::string{} : resolvedPath.generic_string()},
        });
    };
    if (record.sourceMissing || sourceMissing) {
        appendReason("MissingSource", "warning", "Raw source is unavailable. Existing imported metadata and cooked payload may still be usable.", record.sourcePath, sourcePath);
    }
    if (record.importedMetadataMissing || importedMissing) {
        appendReason("MissingImportedMetadata", "error", "Imported transparent asset metadata is missing.", record.importedPath, importedPath);
    }
    if (record.cookedPayloadMissing || payloadMissing) {
        appendReason("MissingCookedPayload", "error", "Cooked/runtime payload is missing.", record.cachePath, cachePath);
    }
    if (record.dependenciesMissing) {
        appendReason("MissingDependencyRecord", "error", "One or more dependency GUID records are missing from the loaded asset registry.", {}, {});
    }
    if (record.missing || record.status == AssetImportStatus::Missing) {
        appendReason("MissingRegistryAsset", "error", "Registry status marks this asset as missing or broken.", {}, {});
    }

    nlohmann::json availableActions = nlohmann::json::array();
    availableActions.push_back({
        {"action", "RevealMetadata"},
        {"available", !importedPath.empty() && regularFileExists(importedPath)},
        {"detail", "Reveal imported metadata when it exists."},
    });
    availableActions.push_back({
        {"action", "RevealPayload"},
        {"available", !cachePath.empty() && regularFileExists(cachePath)},
        {"detail", "Reveal cooked/runtime payload when it exists."},
    });
    availableActions.push_back({
        {"action", "RelinkSource"},
        {"available", true},
        {"detail", "Choose a replacement raw source path for this registry record."},
    });
    availableActions.push_back({
        {"action", "RebuildPayload"},
        {"available", !record.sourcePath.empty() && regularFileExists(sourcePath)},
        {"detail", "Queue reimport to regenerate missing metadata or cooked/runtime payloads when source is available."},
    });

    return {
        {"version", 1},
        {"kind", "SelectedAssetBrokenPlaceholderReport"},
        {"targetGuid", record.guid},
        {"selectedAsset", assetRecordSummaryJson(state, record)},
        {"placeholderRequired", broken},
        {"placeholderKind", broken ? "BrokenAsset" : "None"},
        {"missingReasons", missingReasons},
        {"availableActions", availableActions},
        {"placementPolicy", broken
            ? "Placement should show a broken asset placeholder or block placement until metadata/payload references are repaired."
            : "No broken placeholder is required for the currently loaded registry health state."},
        {"limitation", "This report describes loaded registry health and filesystem availability for the selected record. It does not create a runtime placeholder mesh or repair files automatically."},
    };
}

bool writeAssetRelationshipReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    bool referencesReport,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (state.assetRegistry == nullptr) {
        outError = "Asset registry is unavailable.";
        return false;
    }
    outPath = selectedAssetRelationshipReportPath(state, browserRoot, targetGuid, referencesReport ? "references" : "dependencies");
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset relationship report: " + outPath.string();
        return false;
    }
    file << (referencesReport ? buildAssetReferenceReport(state, targetGuid) : buildAssetDependencyReport(state, targetGuid)).dump(2);
    return true;
}

bool writeAssetDeleteReadinessReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (state.assetRegistry == nullptr) {
        outError = "Asset registry is unavailable.";
        return false;
    }
    outPath = selectedAssetDeleteReadinessReportPath(state, browserRoot, targetGuid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset delete-readiness report: " + outPath.string();
        return false;
    }
    file << buildAssetDeleteReadinessReport(state, browserRoot, targetGuid).dump(2);
    return true;
}

bool writeAssetProjectReferenceScanReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetProjectReferenceReportPath(state, browserRoot, targetGuid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset project reference scan report: " + outPath.string();
        return false;
    }
    file << buildAssetProjectReferenceScanReport(state, browserRoot, targetGuid).dump(2);
    return true;
}

bool writeAssetBrokenPlaceholderReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetRecord& record,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetBrokenPlaceholderReportPath(state, browserRoot, record.guid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset broken-placeholder report: " + outPath.string();
        return false;
    }
    file << buildAssetBrokenPlaceholderReport(state, record).dump(2);
    return true;
}

bool writeAssetValidationReport(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, std::filesystem::path& outPath, std::string& outError, const AssetGuid& targetGuid = {}) {
    if (state.assetRegistry == nullptr) {
        outError = "Asset registry is unavailable.";
        return false;
    }
    outPath = targetGuid.empty() ? assetValidationReportPath(state, browserRoot) : selectedAssetValidationReportPath(state, browserRoot, targetGuid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset validation report: " + outPath.string();
        return false;
    }
    file << buildAssetValidationReport(state, browserRoot, targetGuid).dump(2);
    return true;
}

} // namespace

void AssetBrowserPanel::invalidateThumbnails() {
    thumbnailCache_.clear();
    sourcePreviewCache_.clear();
}

bool AssetBrowserPanel::openSelectedAsset(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    showDetails_ = true;
    if (!selectedRecordGuid_.empty() && state.assetRegistry != nullptr) {
        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (record.guid != selectedRecordGuid_) {
                continue;
            }
            if (record.type == AssetType::Material) {
                if (std::optional<uint32_t> materialIndex = loadedMaterialIndexForRecord(state, record)) {
                    selection.selectMaterial(*materialIndex);
                    requests.showMaterialEditor = true;
                    status_ = "Opened material: " + (record.displayName.empty() ? record.guid : record.displayName);
                } else {
                    requests.showMaterialEditor = true;
                    status_ = "Opened Material Editor; selected material is not loaded in the current scene.";
                }
                return true;
            }
            if (record.type == AssetType::Prefab || record.type == AssetType::Mesh || record.type == AssetType::Scene) {
                status_ = "Opened asset details: " + (record.displayName.empty() ? record.guid : record.displayName);
                return true;
            }
            status_ = "Opened asset details: " + (record.displayName.empty() ? record.guid : record.displayName);
            return true;
        }
        status_ = "Selected asset record is no longer available.";
        return false;
    }
    if (!selectedPath_.empty()) {
        if (std::filesystem::is_directory(selectedPath_)) {
            navigateTo(selectedPath_);
            status_ = "Opened folder: " + selectedPath_.filename().string();
        } else if (isMaterialAssetPath(selectedPath_)) {
            requests.showMaterialEditor = true;
            status_ = "Opened Material Editor for selected material file.";
        } else {
            status_ = "Opened asset details: " + selectedPath_.filename().string();
        }
        return true;
    }
    status_ = "Select an asset in Content before using Open Asset.";
    return false;
}

AssetBrowserPanel::CpuThumbnail& AssetBrowserPanel::thumbnailForPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::string key = ec ? path.string() : absolute.string();
    CpuThumbnail& thumbnail = thumbnailCache_[key];
    if (thumbnail.attempted) {
        return thumbnail;
    }
    thumbnail.attempted = true;
    thumbnail.columns = 12;
    thumbnail.rows = 7;
    thumbnail.colors.assign(static_cast<size_t>(thumbnail.columns * thumbnail.rows), IM_COL32(32, 38, 46, 255));

    if (!isRasterThumbnailPath(path) || !std::filesystem::exists(path, ec)) {
        return thumbnail;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    const std::string filename = path.string();
    if (lowerString(path.extension().string()) == ".hdr") {
        float* data = stbi_loadf(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (data == nullptr || width <= 0 || height <= 0) {
            if (data != nullptr) {
                stbi_image_free(data);
            }
            return thumbnail;
        }
        thumbnail.width = width;
        thumbnail.height = height;
        thumbnail.available = true;
        for (int row = 0; row < thumbnail.rows; ++row) {
            for (int col = 0; col < thumbnail.columns; ++col) {
                const int sampleX = std::clamp((col * width) / thumbnail.columns + width / (thumbnail.columns * 2), 0, width - 1);
                const int sampleY = std::clamp((row * height) / thumbnail.rows + height / (thumbnail.rows * 2), 0, height - 1);
                const size_t index = (static_cast<size_t>(sampleY) * static_cast<size_t>(width) + static_cast<size_t>(sampleX)) * 4u;
                thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)] = IM_COL32(
                    toneMapHdrChannel(data[index + 0]),
                    toneMapHdrChannel(data[index + 1]),
                    toneMapHdrChannel(data[index + 2]),
                    255);
            }
        }
        stbi_image_free(data);
        return thumbnail;
    }

    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (data == nullptr || width <= 0 || height <= 0) {
        if (data != nullptr) {
            stbi_image_free(data);
        }
        return thumbnail;
    }
    thumbnail.width = width;
    thumbnail.height = height;
    thumbnail.available = true;
    for (int row = 0; row < thumbnail.rows; ++row) {
        for (int col = 0; col < thumbnail.columns; ++col) {
            const int sampleX = std::clamp((col * width) / thumbnail.columns + width / (thumbnail.columns * 2), 0, width - 1);
            const int sampleY = std::clamp((row * height) / thumbnail.rows + height / (thumbnail.rows * 2), 0, height - 1);
            const size_t index = (static_cast<size_t>(sampleY) * static_cast<size_t>(width) + static_cast<size_t>(sampleX)) * 4u;
            thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)] = IM_COL32(data[index + 0], data[index + 1], data[index + 2], 255);
        }
    }
    stbi_image_free(data);
    return thumbnail;
}

bool AssetBrowserPanel::drawRasterThumbnail(const std::filesystem::path& path, ImVec2 min, ImVec2 max, bool selected) {
    if (!isRasterThumbnailPath(path)) {
        return false;
    }
    CpuThumbnail& thumbnail = thumbnailForPath(path);
    if (!thumbnail.available) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    const ImVec2 innerMin(min.x + 4.0f, min.y + 4.0f);
    const ImVec2 innerMax(max.x - 4.0f, max.y - 17.0f);
    const float cellW = (innerMax.x - innerMin.x) / static_cast<float>(thumbnail.columns);
    const float cellH = (innerMax.y - innerMin.y) / static_cast<float>(thumbnail.rows);
    for (int row = 0; row < thumbnail.rows; ++row) {
        for (int col = 0; col < thumbnail.columns; ++col) {
            const ImVec2 cellMin(innerMin.x + static_cast<float>(col) * cellW, innerMin.y + static_cast<float>(row) * cellH);
            const ImVec2 cellMax(innerMin.x + static_cast<float>(col + 1) * cellW + 0.5f, innerMin.y + static_cast<float>(row + 1) * cellH + 0.5f);
            dl->AddRectFilled(cellMin, cellMax, thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)]);
        }
    }
    dl->AddRect(innerMin, innerMax, IM_COL32(255, 255, 255, 42), 1.0f);
    const std::string badge = std::to_string(thumbnail.width) + "x" + std::to_string(thumbnail.height);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 15.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(12, 15, 19, 205), 1.0f);
    dl->AddText(ImVec2(min.x + 8.0f, max.y - 15.0f), IM_COL32(178, 188, 202, 255), badge.c_str());
    dl->AddRect(min, max, selected ? ImGui::GetColorU32(editorActiveRowColor()) : IM_COL32(54, 62, 72, 255), EditorUiMetric::cardRounding);
    return true;
}

bool AssetBrowserPanel::drawGpuSceneTextureThumbnail(const EditorRuntimeState& state, const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    if (!state.uiTextures.valid()) {
        return false;
    }
    const std::optional<uint32_t> slot = materialTextureSlotForPath(state, path);
    if (!slot.has_value()) {
        return false;
    }
    const std::vector<VkDescriptorImageInfo> descriptors = state.renderer.scene().materialCombinedDescriptors();
    if (*slot >= descriptors.size()) {
        return false;
    }
    const VkDescriptorImageInfo& descriptor = descriptors[*slot];
    const VkDescriptorSet texture = state.uiTextures.texture(descriptor.imageView, descriptor.imageLayout);
    if (texture == VK_NULL_HANDLE) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    const ImVec2 imageMin(min.x + 4.0f, min.y + 4.0f);
    const ImVec2 imageMax(max.x - 4.0f, max.y - 17.0f);
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)), imageMin, imageMax);
    dl->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 42), 1.0f);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 15.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(12, 15, 19, 205), 1.0f);
    dl->AddText(ImVec2(min.x + 8.0f, max.y - 15.0f), IM_COL32(160, 210, 255, 255), "GPU texture");
    dl->AddRect(min, max, ImGui::GetColorU32(editorActiveRowColor()), EditorUiMetric::cardRounding);
    return true;
}

bool AssetBrowserPanel::drawStandaloneGpuAssetPreview(const EditorRuntimeState& state, const std::filesystem::path& path, ImVec2 min, ImVec2 max, bool selected) {
    uint32_t width = 0;
    uint32_t height = 0;
    const VkDescriptorSet texture = state.uiTextures.assetPreviewTexture(path, &width, &height);
    if (texture == VK_NULL_HANDLE) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    const ImVec2 imageMin(min.x + 4.0f, min.y + 4.0f);
    const ImVec2 imageMax(max.x - 4.0f, max.y - 17.0f);
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)), imageMin, imageMax);
    dl->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 42), 1.0f);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 15.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(12, 15, 19, 205), 1.0f);
    const std::string badge = width > 0 && height > 0
        ? "GPU preview " + std::to_string(width) + "x" + std::to_string(height)
        : std::string("GPU preview");
    dl->AddText(ImVec2(min.x + 8.0f, max.y - 15.0f), IM_COL32(160, 210, 255, 255), badge.c_str());
    dl->AddRect(min, max, selected ? ImGui::GetColorU32(editorActiveRowColor()) : IM_COL32(56, 66, 82, 210), EditorUiMetric::cardRounding);
    return true;
}

std::filesystem::path AssetBrowserPanel::generatedPreviewCachePath(const std::filesystem::path& path) const {
    if (cacheRoot_.empty() || !isGeneratedPreviewDiskCacheCandidate(path)) {
        return {};
    }
    const std::filesystem::path canonical = canonicalForCompare(path);
    const std::string keyText = canonical.string() + "|" + std::to_string(pathWriteStamp(path)) + "|" + std::to_string(pathSizeForCache(path));
    return cacheRoot_ / "Editor" / "GeneratedPreviews" / (hex64(fnv1a64(keyText)) + ".json");
}

bool AssetBrowserPanel::loadGeneratedPreviewDiskCache(const std::filesystem::path& path, SourcePreview& preview) const {
    const std::filesystem::path cachePath = generatedPreviewCachePath(path);
    if (cachePath.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec)) {
        return false;
    }
    std::optional<nlohmann::json> json = readJsonFile(cachePath);
    if (!json.has_value() || json->value("schema", std::string()) != "vibode.generatedPreview.v1") {
        return false;
    }
    const std::filesystem::path canonical = canonicalForCompare(path);
    if (json->value("sourcePath", std::string()) != canonical.string() || json->value("writeStamp", int64_t{}) != pathWriteStamp(path) ||
        json->value("sourceSize", uint64_t{}) != static_cast<uint64_t>(pathSizeForCache(path))) {
        return false;
    }

    preview.available = json->value("available", false);
    preview.loadedFromDiskCache = true;
    preview.icon = editorGlyphForPath(path);
    preview.title = json->value("title", path.filename().string());
    preview.kind = json->value("kind", contentKindLabel(path));
    preview.lines.clear();
    if (json->contains("lines") && (*json)["lines"].is_array()) {
        for (const nlohmann::json& line : (*json)["lines"]) {
            if (line.is_string()) {
                preview.lines.push_back(line.get<std::string>());
            }
        }
    }
    return preview.available;
}

void AssetBrowserPanel::saveGeneratedPreviewDiskCache(const std::filesystem::path& path, const SourcePreview& preview) const {
    const std::filesystem::path cachePath = generatedPreviewCachePath(path);
    if (cachePath.empty() || !preview.available) {
        return;
    }
    const std::filesystem::path parent = cachePath.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return;
        }
    }

    nlohmann::json json;
    json["schema"] = "vibode.generatedPreview.v1";
    json["sourcePath"] = canonicalForCompare(path).string();
    json["writeStamp"] = pathWriteStamp(path);
    json["sourceSize"] = static_cast<uint64_t>(pathSizeForCache(path));
    json["available"] = preview.available;
    json["title"] = preview.title;
    json["kind"] = preview.kind;
    json["lines"] = preview.lines;

    std::ofstream file(cachePath, std::ios::trunc);
    if (!file) {
        return;
    }
    file << json.dump(2);
}

AssetBrowserPanel::SourcePreview& AssetBrowserPanel::sourcePreviewForPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::string key = ec ? path.string() : absolute.string();
    SourcePreview& preview = sourcePreviewCache_[key];
    if (preview.attempted) {
        return preview;
    }

    preview.attempted = true;
    if (loadGeneratedPreviewDiskCache(path, preview)) {
        return preview;
    }

    auto finishPreview = [&]() -> SourcePreview& {
        saveGeneratedPreviewDiskCache(path, preview);
        return preview;
    };

    preview.available = true;
    preview.icon = editorGlyphForPath(path);
    preview.title = path.filename().string();
    preview.kind = contentKindLabel(path);
    preview.lines.push_back(fileSizeLabel(path));

    if (std::filesystem::is_directory(path, ec)) {
        size_t folders = 0;
        size_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory(ec)) {
                ++folders;
            } else if (entry.is_regular_file(ec)) {
                ++files;
            }
        }
        preview.lines.push_back(countLabel("Folders", folders));
        preview.lines.push_back(countLabel("Files", files));
        return finishPreview();
    }

    if (isModelAssetPath(path)) {
        if (lowerString(path.extension().string()) == ".gltf") {
            if (std::optional<nlohmann::json> json = readJsonFile(path)) {
                preview.lines.push_back(countLabel("Nodes", jsonArraySize(*json, "nodes")));
                preview.lines.push_back(countLabel("Meshes", jsonArraySize(*json, "meshes")));
                preview.lines.push_back(countLabel("Materials", jsonArraySize(*json, "materials")));
                preview.lines.push_back(countLabel("Textures", jsonArraySize(*json, "textures")));
                preview.lines.push_back(countLabel("Cameras", jsonArraySize(*json, "cameras")));
            } else {
                preview.lines.push_back("glTF metadata unavailable");
            }
        } else {
            preview.lines.push_back("Binary/external model preview");
            preview.lines.push_back("Import to generate prefab metadata");
        }
        return finishPreview();
    }

    if (isSceneAssetPath(path)) {
        if (std::optional<nlohmann::json> json = readJsonFile(path)) {
            const size_t entities = json->contains("entities") && (*json)["entities"].is_array() ? (*json)["entities"].size() : 0;
            size_t cameras = 0;
            size_t lights = 0;
            size_t meshes = 0;
            if (json->contains("entities") && (*json)["entities"].is_array()) {
                for (const nlohmann::json& entity : (*json)["entities"]) {
                    if (entity.contains("camera")) ++cameras;
                    if (entity.contains("light") || entity.contains("sun")) ++lights;
                    if (entity.contains("meshRenderer")) ++meshes;
                }
            }
            preview.lines.push_back(countLabel("Entities", entities));
            preview.lines.push_back(countLabel("Cameras", cameras));
            preview.lines.push_back(countLabel("Lights", lights));
            preview.lines.push_back(countLabel("Mesh renderers", meshes));
        } else {
            preview.lines.push_back("Scene metadata unavailable");
        }
        return finishPreview();
    }

    if (isProjectAssetPath(path)) {
        if (std::optional<nlohmann::json> json = readJsonFile(path)) {
            preview.title = json->value("name", preview.title);
            preview.lines.push_back("Project file .vproject");
            preview.lines.push_back("Startup " + json->value("startupScene", std::string("(none)")));
            preview.lines.push_back("Content " + json->value("contentRoot", std::string("Content")));
            preview.lines.push_back("Scenes " + json->value("scenesRoot", std::string("Scenes")));
        } else {
            preview.lines.push_back("Project metadata unavailable");
        }
        return finishPreview();
    }

    if (isMaterialAssetPath(path)) {
        std::ifstream file(path);
        size_t materials = 0;
        size_t textureRefs = 0;
        std::string line;
        while (std::getline(file, line)) {
            const std::string lower = lowerString(line);
            if (lower.rfind("newmtl ", 0) == 0) ++materials;
            if (lower.rfind("map_", 0) == 0) ++textureRefs;
        }
        preview.lines.push_back(countLabel("Materials", materials));
        preview.lines.push_back(countLabel("Texture refs", textureRefs));
        preview.lines.push_back("Swatch preview from source metadata");
        return finishPreview();
    }

    if (isIesAssetPath(path)) {
        std::ifstream file(path);
        std::string firstLine;
        std::getline(file, firstLine);
        preview.lines.push_back(firstLine.empty() ? "IES photometric profile" : firstLine.substr(0, 80));
        preview.lines.push_back("Assignable to authored light profile fields");
        return finishPreview();
    }

    if (isVolumeAssetPath(path)) {
        preview.lines.push_back("OpenVDB volume container");
        preview.lines.push_back("Metadata preview; runtime import pending");
        return finishPreview();
    }

    preview.available = supportedContentPath(path);
    if (!preview.available) {
        preview.lines.push_back("Unsupported file type");
    }
    return finishPreview();
}

bool AssetBrowserPanel::drawGeneratedSourcePreview(const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    SourcePreview& preview = sourcePreviewForPath(path);
    if (!preview.available) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 accent = contentIconColor(path);
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, min.y + 4.0f), ImVec2(max.x - 4.0f, min.y + 30.0f), IM_COL32(28, 34, 43, 245), 2.0f);
    editorDrawIconGlyph(preview.icon, ImVec2(min.x + 10.0f, min.y + 8.0f), ImVec2(min.x + 26.0f, min.y + 24.0f), accent);
    dl->AddText(ImVec2(min.x + 32.0f, min.y + 9.0f), IM_COL32(220, 226, 236, 255), preview.kind.c_str());

    const float diagramTop = min.y + 38.0f;
    if (isModelAssetPath(path)) {
        const ImVec2 c(min.x + 42.0f, diagramTop + 32.0f);
        const float s = 28.0f;
        dl->AddRect(ImVec2(c.x - s, c.y - s * 0.55f), ImVec2(c.x + s, c.y + s * 0.55f), accent, 2.0f, 0, 1.5f);
        dl->AddLine(ImVec2(c.x - s, c.y - s * 0.55f), ImVec2(c.x - s * 0.45f, c.y - s), accent, 1.5f);
        dl->AddLine(ImVec2(c.x + s, c.y - s * 0.55f), ImVec2(c.x + s * 0.45f, c.y - s), accent, 1.5f);
        dl->AddLine(ImVec2(c.x - s * 0.45f, c.y - s), ImVec2(c.x + s * 0.45f, c.y - s), accent, 1.5f);
    } else if (isSceneAssetPath(path) || isProjectAssetPath(path)) {
        for (int i = 0; i < 4; ++i) {
            const float x = min.x + 18.0f + static_cast<float>(i % 2) * 42.0f;
            const float y = diagramTop + 8.0f + static_cast<float>(i / 2) * 24.0f;
            dl->AddRect(ImVec2(x, y), ImVec2(x + 28.0f, y + 16.0f), accent, 2.0f, 0, 1.3f);
        }
    } else if (isMaterialAssetPath(path)) {
        dl->AddCircleFilled(ImVec2(min.x + 44.0f, diagramTop + 34.0f), 26.0f, IM_COL32(115, 92, 140, 255));
        dl->AddCircle(ImVec2(min.x + 44.0f, diagramTop + 34.0f), 26.0f, accent, 24, 1.5f);
    } else if (isIesAssetPath(path)) {
        const ImVec2 c(min.x + 48.0f, diagramTop + 42.0f);
        dl->PathLineTo(c);
        for (int i = 0; i <= 12; ++i) {
            const float t = static_cast<float>(i) / 12.0f;
            const float angle = -1.25f + t * 2.5f;
            const float radius = 14.0f + std::sin(t * 3.14159f) * 24.0f;
            dl->PathLineTo(ImVec2(c.x + std::cos(angle) * radius, c.y - std::sin(angle) * radius));
        }
        dl->PathStroke(accent, 0, 1.6f);
    } else if (isVolumeAssetPath(path)) {
        for (int i = 0; i < 4; ++i) {
            dl->AddCircle(ImVec2(min.x + 30.0f + i * 12.0f, diagramTop + 26.0f + (i % 2) * 9.0f), 18.0f, IM_COL32(130, 210, 190, 120), 20, 1.4f);
        }
    }

    float textY = min.y + 38.0f;
    const float textX = min.x + 92.0f;
    const float textMaxX = max.x - 8.0f;
    for (size_t i = 0; i < preview.lines.size() && i < 4; ++i) {
        std::string line = preview.lines[i];
        while (!line.empty() && ImGui::CalcTextSize(line.c_str()).x > textMaxX - textX) {
            line.pop_back();
        }
        if (line.size() < preview.lines[i].size() && line.size() > 3) {
            line.replace(line.size() - 3, 3, "...");
        }
        dl->AddText(ImVec2(textX, textY), i == 0 ? IM_COL32(205, 214, 226, 255) : IM_COL32(142, 151, 164, 255), line.c_str());
        textY += 17.0f;
    }
    dl->AddRect(min, max, IM_COL32(65, 76, 91, 190), EditorUiMetric::cardRounding);
    return true;
}

void AssetBrowserPanel::loadFromPath(const std::filesystem::path& path, EditorRequests& requests) {
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".hdr" || ext == ".exr") {
        requests.loadHdr = path;
        status_ = "Queued HDRI import/apply: " + path.string();
    } else if (ext == ".rtlevel") {
        requests.openScene = path;
        status_ = "Queued scene open: " + path.string();
    } else if (!compatibilityMode_ && isModelAssetPath(path)) {
        requests.importAsset = EditorImportAssetRequest{.sourcePath = path};
        recordImportOperation("Import Asset", path, currentPath_, "ImportAsset");
        status_ = "Queued Import Asset: " + path.string();
    } else {
        requests.importSceneAsNewScene = path;
        status_ = "Queued Import Scene as New Scene: " + path.string();
    }
}

void AssetBrowserPanel::recordImportOperation(
    const std::string& label,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationFolder,
    const std::string& mode,
    const AssetGuid& assetGuid) {
    for (ImportOperation& operation : importOperations_) {
        const bool sameGuid = !assetGuid.empty() && operation.assetGuid == assetGuid;
        const bool sameSource = assetGuid.empty() && samePathForOperation(operation.sourcePath, sourcePath) && operation.mode == mode;
        if ((sameGuid || sameSource) && !operation.completed && !operation.failed) {
            operation.label = label;
            operation.sourcePath = sourcePath;
            operation.destinationFolder = destinationFolder;
            operation.mode = mode;
            operation.progress = std::max(operation.progress, 0.12f);
            operation.state = "Queued";
            return;
        }
    }

    ImportOperation operation;
    operation.id = nextImportOperationId_++;
    operation.label = label;
    operation.sourcePath = sourcePath;
    operation.destinationFolder = destinationFolder;
    operation.mode = mode;
    operation.assetGuid = assetGuid;
    operation.progress = 0.12f;
    operation.state = "Queued";
    importOperations_.insert(importOperations_.begin(), std::move(operation));
    if (importOperations_.size() > 12) {
        importOperations_.resize(12);
    }
}

void AssetBrowserPanel::refreshImportOperations(const EditorRuntimeState& state) {
    if (importOperations_.empty()) {
        return;
    }
    const AssetRegistry* registry = state.assetRegistry;
    for (ImportOperation& operation : importOperations_) {
        if (operation.completed || operation.failed) {
            continue;
        }

        const AssetRecord* matchedRecord = nullptr;
        if (registry != nullptr) {
            for (const AssetRecord& record : registry->records()) {
                if (!operation.assetGuid.empty() && record.guid == operation.assetGuid) {
                    matchedRecord = &record;
                    break;
                }
                const std::filesystem::path recordSource = resolveAssetRecordPath(state, record.sourcePath);
                if (operation.assetGuid.empty() && samePathForOperation(operation.sourcePath, recordSource)) {
                    matchedRecord = &record;
                    break;
                }
            }
        }

        if (matchedRecord != nullptr) {
            operation.progress = assetImportProgress(*matchedRecord);
            operation.state = assetImportProgressLabel(*matchedRecord);
            operation.failed = matchedRecord->status == AssetImportStatus::Failed;
            operation.completed = matchedRecord->status == AssetImportStatus::Imported && !matchedRecord->missing && !matchedRecord->stale;
            if (operation.completed) {
                operation.progress = 1.0f;
                operation.state = "Completed";
            }
        } else {
            operation.progress = std::max(operation.progress, 0.35f);
            operation.state = "Handed off";
        }
    }
}

void AssetBrowserPanel::drawImportOperations() {
    if (importOperations_.empty()) {
        return;
    }

    ImGui::SeparatorText("Import Queue");
    ImGui::TextDisabled("Queued and recent asset import operations");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Completed")) {
        importOperations_.erase(
            std::remove_if(importOperations_.begin(), importOperations_.end(), [](const ImportOperation& operation) {
                return operation.completed || operation.failed;
            }),
            importOperations_.end());
    }

    if (ImGui::BeginTable("ContentImportOperations", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Operation");
        ImGui::TableSetupColumn("Asset");
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::progressColumnWidth);
        ImGui::TableHeadersRow();
        for (const ImportOperation& operation : importOperations_) {
            ImGui::PushID(static_cast<int>(operation.id));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(operation.label.c_str());
            ImGui::TableSetColumnIndex(1);
            const std::string assetName = operation.sourcePath.empty()
                ? operation.assetGuid
                : operation.sourcePath.filename().string();
            ImGui::TextUnformatted(assetName.empty() ? "(asset)" : assetName.c_str());
            if (!operation.destinationFolder.empty()) {
                ImGui::TextDisabled("-> %s", operation.destinationFolder.generic_string().c_str());
            }
            ImGui::TableSetColumnIndex(2);
            if (operation.failed) {
                ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.32f, 1.0f), "%s", operation.state.c_str());
            } else if (operation.completed) {
                ImGui::TextColored(ImVec4(0.48f, 0.82f, 0.55f, 1.0f), "%s", operation.state.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.55f, 0.72f, 0.95f, 1.0f), "%s", operation.state.c_str());
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::ProgressBar(std::clamp(operation.progress, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f), operation.completed ? "Done" : operation.state.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void AssetBrowserPanel::prepareImportDialog(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationFolder, int mode) {
    setPathBuffer(importSourcePath_, sourcePath);
    std::string destination = "Models";
    if (!destinationFolder.empty()) {
        destination = relativeImportDestination(destinationFolder);
        if (destination.empty()) {
            destination = ".";
        }
    }
    setTextBuffer(importDestinationFolder_, destination);
    importMode_ = mode;
    openImportSettings_ = true;
}

void AssetBrowserPanel::syncBrowserRoot(const EditorRuntimeState& state) {
    std::filesystem::path root;
    std::filesystem::path defaultPath;
    compatibilityMode_ = state.project == nullptr;
    if (state.project != nullptr) {
        root = state.project->projectRoot;
        defaultPath = state.project->contentRoot;
        contentRoot_ = canonicalForCompare(state.project->contentRoot);
        scenesRoot_ = canonicalForCompare(state.project->scenesRoot);
        savedRoot_ = canonicalForCompare(state.project->savedRoot);
        cacheRoot_ = canonicalForCompare(state.project->cacheRoot);
    } else if (state.scenePath != nullptr && state.scenePath->has_value()) {
        root = state.scenePath->value().parent_path();
        defaultPath = root;
        contentRoot_.clear();
        scenesRoot_.clear();
        savedRoot_.clear();
        cacheRoot_.clear();
    } else {
        root = std::filesystem::current_path();
        defaultPath = root;
        contentRoot_.clear();
        scenesRoot_.clear();
        savedRoot_.clear();
        cacheRoot_.clear();
    }

    if (root.empty()) {
        root = std::filesystem::current_path();
    }
    if (defaultPath.empty()) {
        defaultPath = root;
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    std::filesystem::create_directories(defaultPath, ec);
    root = canonicalForCompare(root);
    defaultPath = canonicalForCompare(defaultPath);
    if (browserRoot_ != root) {
        browserRoot_ = root;
        currentPath_ = pathIsWithin(defaultPath, browserRoot_) ? defaultPath : root;
        selectedPath_.clear();
        selectedRecordGuid_.clear();
        backStack_.clear();
        forwardStack_.clear();
        sourceControlStatusCache_.clear();
        clearGitStatusSnapshots();
    } else if (currentPath_.empty()) {
        currentPath_ = root;
    }
}

void AssetBrowserPanel::navigateTo(const std::filesystem::path& path, bool addHistory) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_directory(path, ec)) {
        return;
    }
    const std::filesystem::path next = canonicalForCompare(path);
    if (next == currentPath_) {
        return;
    }
    if (addHistory && !currentPath_.empty()) {
        backStack_.push_back(currentPath_);
        forwardStack_.clear();
    }
    currentPath_ = next;
    selectedPath_.clear();
    selectedRecordGuid_.clear();
}

bool AssetBrowserPanel::shouldShowPath(const std::filesystem::path& path) const {
    const std::string filter = lowerString(search_.data());
    if (filter.empty()) {
        return true;
    }
    return lowerString(path.filename().string()).find(filter) != std::string::npos;
}

std::string AssetBrowserPanel::relativeContentPath(const std::filesystem::path& path) const {
    if (browserRoot_.empty() || path.empty()) {
        return path.string();
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, browserRoot_, ec);
    return ec ? path.string() : relative.generic_string();
}

std::string AssetBrowserPanel::relativeImportDestination(const std::filesystem::path& path) const {
    if (!contentRoot_.empty() && pathIsWithin(path, contentRoot_)) {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(path, contentRoot_, ec);
        return ec ? path.string() : relative.generic_string();
    }
    return relativeContentPath(path);
}

void AssetBrowserPanel::drawPathContextMenu(const std::filesystem::path& path, bool isDirectory, EditorRequests& requests) {
    if (isDirectory) {
        if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Open Folder")) {
            navigateTo(path);
        }
        editorGlyphMenuItem(EditorGlyphIcon::Add, "New Folder", false);
        if (editorGlyphMenuItem(EditorGlyphIcon::Import, "Import Here...", !compatibilityMode_)) {
            if (auto source = openImportAssetFileDialog()) {
                prepareImportDialog(*source, path, 0);
            }
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Import and Place Here...", !compatibilityMode_)) {
            if (auto source = openGltfFileDialog()) {
                prepareImportDialog(*source, path, 1);
            }
        }
        ImGui::Separator();
        if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy Path")) {
            copyPathToClipboard(path);
            status_ = "Copied path: " + path.string();
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Show in Explorer")) {
            revealPathInFileBrowser(path);
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Refresh, "Refresh")) {
            invalidateThumbnails();
            status_ = "Content refreshed";
        }
        return;
    }

    const bool canOpen = canOpenOrApplyPath(path);
    const bool canImport = !compatibilityMode_ && isImportableSourceAssetPath(path);
    const bool canImportAndPlace = !compatibilityMode_ && isPlaceablePrefabSourcePath(path);
    if (editorGlyphMenuItem(editorGlyphForPath(path), "Open / Apply", canOpen)) {
        loadFromPath(path, requests);
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Import, "Import Asset...", canImport)) {
        prepareImportDialog(path, currentPath_, 0);
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Import and Place...", canImportAndPlace)) {
        prepareImportDialog(path, currentPath_, 1);
    }
    editorGlyphMenuItem(EditorGlyphIcon::Details, "Preview", false);
    editorGlyphMenuItem(EditorGlyphIcon::Refresh, "Reimport", false);
    ImGui::Separator();
    if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy Path")) {
        copyPathToClipboard(path);
        status_ = "Copied path: " + path.string();
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Show in Explorer")) {
        revealPathInFileBrowser(path);
    }
    editorGlyphMenuItem(EditorGlyphIcon::Trash, "Delete", false);
}

void AssetBrowserPanel::drawFolderTree(const std::filesystem::path& path, EditorRequests& requests) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        return;
    }
    const bool selected = canonicalForCompare(path) == canonicalForCompare(currentPath_);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    bool hasChildren = false;
    for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_directory(ec)) {
            hasChildren = true;
            break;
        }
    }
    if (!hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    ImGui::PushID(path.string().c_str());
    const std::string treeLabel = editorGlyphLabel(path.filename().empty() ? path.string() : path.filename().string());
    editorDrawPreRowBand(EditorUiMetric::contentRowHeight);
    editorPushRowSelectionStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, editorRowFramePadding(EditorUiMetric::contentRowHeight));
    const bool open = ImGui::TreeNodeEx(treeLabel.c_str(), flags);
    ImGui::PopStyleVar();
    editorPopRowSelectionStyle();
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const float iconX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing() + 2.0f;
    const float iconY = rowMin.y + std::max(0.0f, (rowMax.y - rowMin.y - 16.0f) * 0.5f);
    drawContentGlyph(path, ImVec2(iconX, iconY), ImVec2(iconX + 16.0f, iconY + 16.0f));
    if (ImGui::IsItemClicked()) {
        navigateTo(path);
    }
    if (ImGui::BeginPopupContextItem("FolderContext")) {
        selectedPath_ = path;
        selectedRecordGuid_.clear();
        drawPathContextMenu(path, true, requests);
        ImGui::EndPopup();
    }
    if (open) {
        std::vector<std::filesystem::path> children;
        for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory(ec)) {
                children.push_back(entry.path());
            }
        }
        std::sort(children.begin(), children.end());
        for (const auto& child : children) {
            drawFolderTree(child, requests);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetBrowserPanel::drawPathList(const EditorRuntimeState& state, EditorRequests& requests) {
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> entries;
    if (!currentPath_.empty() && std::filesystem::is_directory(currentPath_, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(currentPath_, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (shouldShowPath(entry.path())) {
                entries.push_back(entry);
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        std::error_code errorA;
        std::error_code errorB;
        const bool aDir = a.is_directory(errorA);
        const bool bDir = b.is_directory(errorB);
        if (aDir != bDir) {
            return aDir > bDir;
        }
        return lowerString(a.path().filename().string()) < lowerString(b.path().filename().string());
    });

    if (gridView_) {
        const float cellWidth = EditorUiMetric::contentGridCellWidth;
        const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cellWidth));
        ImGui::Columns(columns, "ContentGrid", false);
        for (const auto& entry : entries) {
            const std::filesystem::path path = entry.path();
            const bool selected = selectedPath_ == path;
            ImGui::PushID(path.string().c_str());
            const ImVec2 thumbSize(EditorUiMetric::contentGridThumbWidth, EditorUiMetric::contentGridThumbHeight);
            ImGui::InvisibleButton("ContentGridThumb", thumbSize);
            const ImVec2 thumbMin = ImGui::GetItemRectMin();
            const ImVec2 thumbMax = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(thumbMin, thumbMax, selected ? ImGui::GetColorU32(editorSelectedRowColor()) : IM_COL32(24, 27, 32, 255), EditorUiMetric::cardRounding);
            dl->AddRect(thumbMin, thumbMax, selected ? ImGui::GetColorU32(editorActiveRowColor()) : IM_COL32(54, 62, 72, 255), EditorUiMetric::cardRounding);
            if (!drawGpuSceneTextureThumbnail(state, path, thumbMin, thumbMax) &&
                !drawStandaloneGpuAssetPreview(state, path, thumbMin, thumbMax, selected) &&
                !drawRasterThumbnail(path, thumbMin, thumbMax, selected)) {
                drawContentGlyph(
                    path,
                    ImVec2(thumbMin.x + thumbSize.x * 0.34f, thumbMin.y + thumbSize.y * 0.22f),
                    ImVec2(thumbMax.x - thumbSize.x * 0.34f, thumbMax.y - thumbSize.y * 0.22f));
            }
            if (ImGui::IsItemClicked()) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (entry.is_directory(ec)) {
                    navigateTo(path);
                } else {
                    loadFromPath(path, requests);
                }
            }
            if (ImGui::BeginPopupContextItem("PathContext")) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
                drawPathContextMenu(path, entry.is_directory(ec), requests);
                ImGui::EndPopup();
            }
            ImGui::TextWrapped("%s%s", selected ? "> " : "", path.filename().string().c_str());
            ImGui::NextColumn();
            ImGui::PopID();
        }
        ImGui::Columns(1);
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 0.0f));
    if (ImGui::BeginTable("ContentPathListCompact", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Asset");
        for (const auto& entry : entries) {
            const std::filesystem::path path = entry.path();
            const bool isDir = entry.is_directory(ec);
            const bool selected = selectedPath_ == path;
            ImGui::PushID(path.string().c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const ImVec2 nameCursor = ImGui::GetCursorScreenPos();
            const std::string name = editorGlyphLabel(path.filename().string());
            editorPushRowSelectionStyle();
            if (ImGui::Selectable(
                    name.c_str(),
                    selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0.0f, EditorUiMetric::contentRowHeight))) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (isDir) {
                        navigateTo(path);
                    } else {
                        loadFromPath(path, requests);
                    }
                }
            }
            editorPopRowSelectionStyle();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float iconY = nameCursor.y + std::max(0.0f, (itemMax.y - nameCursor.y - 16.0f) * 0.5f);
            drawContentGlyph(path, ImVec2(nameCursor.x + 2.0f, iconY), ImVec2(nameCursor.x + 18.0f, iconY + 16.0f));
            if (ImGui::BeginPopupContextItem("PathContext")) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
                drawPathContextMenu(path, isDir, requests);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void AssetBrowserPanel::drawRegistryTable(const EditorRuntimeState& state, EditorRequests& requests) {
    if (state.assetRegistry == nullptr) {
        return;
    }
    const AssetRegistry& registry = *state.assetRegistry;
    ImGui::SeparatorText("Asset Registry");
    ImGui::Text("Registry: %s%s",
        registry.state().path.empty() ? "(none)" : registry.state().path.string().c_str(),
        registry.dirty() ? " *" : "");
    const auto& records = registry.records();
    if (records.empty()) {
        ImGui::TextDisabled("No registry records yet. Import Asset will populate this in the next milestone.");
        return;
    }
    const std::vector<std::string> registryTags = collectRegistryTags(state.assetRegistry);
    const std::vector<std::string> tagSuggestions = mergedTagSuggestions(registryTags, state.editorPrefs);

    constexpr const char* typeFilters[] = {"All Types", "Mesh", "Material", "Texture", "HDRI", "Scene", "Prefab", "Unknown"};
    constexpr const char* statusFilters[] = {"All Status", "Imported", "Missing", "Stale", "Failed", "Unknown"};
    constexpr const char* healthFilters[] = {
        "All Health",
        "Healthy",
        "Any Issue",
        "Source Missing",
        "Metadata Missing",
        "Payload Missing",
        "Dependency Missing",
        "Has Dependencies",
        "Has References",
        "Used By Loaded Data",
    };
    constexpr const char* favoriteFilters[] = {"All Assets", "Favorite Assets"};
    registryTypeFilter_ = std::clamp(registryTypeFilter_, 0, static_cast<int>(std::size(typeFilters)) - 1);
    registryStatusFilter_ = std::clamp(registryStatusFilter_, 0, static_cast<int>(std::size(statusFilters)) - 1);
    registryHealthFilter_ = std::clamp(registryHealthFilter_, 0, static_cast<int>(std::size(healthFilters)) - 1);
    registryFavoriteFilter_ = std::clamp(registryFavoriteFilter_, 0, static_cast<int>(std::size(favoriteFilters)) - 1);
    const int collectionFilterMax = state.editorPrefs != nullptr ? static_cast<int>(state.editorPrefs->assetCollections.size()) : 0;
    registryCollectionFilter_ = std::clamp(registryCollectionFilter_, 0, collectionFilterMax);

    ImGui::PushID("RegistryFilters");
    ImGui::SetNextItemWidth(126.0f);
    ImGui::Combo("##type", &registryTypeFilter_, typeFilters, static_cast<int>(std::size(typeFilters)));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Filter registry records by asset type");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(126.0f);
    ImGui::Combo("##status", &registryStatusFilter_, statusFilters, static_cast<int>(std::size(statusFilters)));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Filter registry records by import status");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(164.0f);
    ImGui::Combo("##health", &registryHealthFilter_, healthFilters, static_cast<int>(std::size(healthFilters)));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Filter registry records by health, dependencies, or loaded usage");
    }
    if (state.editorPrefs != nullptr) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(132.0f);
        ImGui::Combo("##favorites", &registryFavoriteFilter_, favoriteFilters, static_cast<int>(std::size(favoriteFilters)));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Filter registry records to favorite assets");
        }
    }
    if (state.editorPrefs != nullptr && !state.editorPrefs->assetCollections.empty()) {
        ImGui::SameLine();
        const char* collectionLabel = "All Collections";
        if (const EditorAssetCollection* collection = selectedCollection(state.editorPrefs, registryCollectionFilter_)) {
            collectionLabel = collection->name.c_str();
        }
        if (ImGui::BeginCombo("##collectionFilter", collectionLabel)) {
            if (ImGui::Selectable("All Collections", registryCollectionFilter_ == 0)) {
                registryCollectionFilter_ = 0;
            }
            for (size_t i = 0; i < state.editorPrefs->assetCollections.size(); ++i) {
                const int index = static_cast<int>(i + 1);
                const EditorAssetCollection& collection = state.editorPrefs->assetCollections[i];
                if (ImGui::Selectable(collection.name.c_str(), registryCollectionFilter_ == index)) {
                    registryCollectionFilter_ = index;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Filter registry records by saved asset collection");
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(132.0f);
    ImGui::InputTextWithHint("##tag", "Tag filter", registryTagFilter_.data(), registryTagFilter_.size());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Filter registry records by asset tag");
    }
    if (!tagSuggestions.empty()) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("##tagPresetFilter", "Tags")) {
            for (const std::string& tag : tagSuggestions) {
                const bool selected = lowerString(trimString(registryTagFilter_.data())) == lowerString(tag);
                if (ImGui::Selectable(tag.c_str(), selected)) {
                    setTextBuffer(registryTagFilter_, tag);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Use an existing asset tag as the filter");
        }
    }
    const bool filtersActive = registryTypeFilter_ != 0 || registryStatusFilter_ != 0 || registryHealthFilter_ != 0 || registryCollectionFilter_ != 0 || registryFavoriteFilter_ != 0 || registryTagFilter_[0] != '\0' || search_[0] != '\0';
    if (filtersActive) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Filters")) {
            registryTypeFilter_ = 0;
            registryStatusFilter_ = 0;
            registryHealthFilter_ = 0;
            registryCollectionFilter_ = 0;
            registryFavoriteFilter_ = 0;
            std::fill(registryTagFilter_.begin(), registryTagFilter_.end(), '\0');
            std::fill(search_.begin(), search_.end(), '\0');
        }
    }
    ImGui::PopID();

    auto typeMatches = [&](AssetType type) {
        switch (registryTypeFilter_) {
        case 1: return type == AssetType::Mesh;
        case 2: return type == AssetType::Material;
        case 3: return type == AssetType::Texture;
        case 4: return type == AssetType::HDRI;
        case 5: return type == AssetType::Scene;
        case 6: return type == AssetType::Prefab;
        case 7: return type == AssetType::Unknown;
        default: return true;
        }
    };
    auto statusMatches = [&](AssetImportStatus status) {
        switch (registryStatusFilter_) {
        case 1: return status == AssetImportStatus::Imported;
        case 2: return status == AssetImportStatus::Missing;
        case 3: return status == AssetImportStatus::Stale;
        case 4: return status == AssetImportStatus::Failed;
        case 5: return status == AssetImportStatus::Unknown;
        default: return true;
        }
    };
    auto searchMatches = [&](const AssetRecord& record) {
        const std::string filter = lowerString(search_.data());
        if (filter.empty()) {
            return true;
        }
        return lowerString(record.displayName).find(filter) != std::string::npos ||
            lowerString(record.guid).find(filter) != std::string::npos ||
            lowerString(record.sourcePath).find(filter) != std::string::npos ||
            lowerString(record.importedPath).find(filter) != std::string::npos ||
            lowerString(record.cachePath).find(filter) != std::string::npos ||
            lowerString(joinTagList(record.tags)).find(filter) != std::string::npos;
    };
    auto healthMatches = [&](const AssetRecord& record) {
        const AssetUsageSummary usage = registryHealthFilter_ == 9 ? assetUsageSummaryForRecord(state, record) : AssetUsageSummary{};
        switch (registryHealthFilter_) {
        case 1: return !record.missing && !record.stale && record.status == AssetImportStatus::Imported;
        case 2: return record.missing || record.stale || record.sourceMissing || record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing || record.status == AssetImportStatus::Missing || record.status == AssetImportStatus::Stale || record.status == AssetImportStatus::Failed;
        case 3: return record.sourceMissing;
        case 4: return record.importedMetadataMissing;
        case 5: return record.cookedPayloadMissing;
        case 6: return record.dependenciesMissing;
        case 7: return !record.dependencies.empty();
        case 8: return !record.references.empty();
        case 9: return usage.referenced();
        default: return true;
        }
    };
    auto recordMatchesFilters = [&](const AssetRecord& record) {
        const EditorAssetCollection* collection = selectedCollection(state.editorPrefs, registryCollectionFilter_);
        const bool collectionMatches = collection == nullptr || collectionContainsAsset(*collection, record.guid);
        const bool favoriteMatches = registryFavoriteFilter_ == 0 || (state.editorPrefs != nullptr && assetGuidListContains(state.editorPrefs->favoriteAssetGuids, record.guid));
        return typeMatches(record.type) && statusMatches(record.status) && healthMatches(record) && collectionMatches && favoriteMatches && recordHasTagMatch(record, registryTagFilter_.data()) && searchMatches(record);
    };

    std::vector<AssetGuid> visibleRecordGuids;
    visibleRecordGuids.reserve(records.size());
    for (const AssetRecord& record : records) {
        if (recordMatchesFilters(record)) {
            visibleRecordGuids.push_back(record.guid);
        }
    }
    const size_t visibleRecordCount = visibleRecordGuids.size();
    ImGui::TextDisabled("Showing %zu of %zu registry records", visibleRecordCount, records.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputTextWithHint("##collectionName", "Collection", collectionNameBuffer_.data(), collectionNameBuffer_.size());
    if (state.editorPrefs != nullptr && !state.editorPrefs->assetCollections.empty()) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("##collectionPicker", "Collections")) {
            for (const EditorAssetCollection& collection : state.editorPrefs->assetCollections) {
                if (ImGui::Selectable(collection.name.c_str(), false)) {
                    setTextBuffer(collectionNameBuffer_, collection.name);
                }
            }
            ImGui::EndCombo();
        }
    }
    const std::string collectionName = trimString(collectionNameBuffer_.data());
    const bool canEditCollection = state.editorPrefs != nullptr && !collectionName.empty();
    auto savePrefsStatus = [&](std::string successMessage, std::string failureDetail) {
        if (state.editorPrefs == nullptr) {
            return;
        }
        setPreferenceSaveStatus(state.editorPrefs->save(EditorPreferences::defaultPath()), status_, std::move(successMessage), std::move(failureDetail));
    };
    ImGui::SameLine();
    if (!canEditCollection || visibleRecordGuids.empty()) {
        ImGui::BeginDisabled();
    }
    if (contentActionButton("AddVisibleToCollection", EditorGlyphIcon::Add, "Add Visible", "Add currently visible registry records to this collection")) {
        state.editorPrefs->addAssetsToCollection(collectionName, visibleRecordGuids);
        savePrefsStatus("Added visible assets to collection: " + collectionName, "add visible assets to collection " + collectionName);
    }
    if (!canEditCollection || visibleRecordGuids.empty()) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!canEditCollection || visibleRecordGuids.empty()) {
        ImGui::BeginDisabled();
    }
    if (contentActionButton("RemoveVisibleFromCollection", EditorGlyphIcon::Trash, "Remove Visible", "Remove currently visible registry records from this collection")) {
        state.editorPrefs->removeAssetsFromCollection(collectionName, visibleRecordGuids);
        savePrefsStatus("Removed visible assets from collection: " + collectionName, "remove visible assets from collection " + collectionName);
    }
    if (!canEditCollection || visibleRecordGuids.empty()) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!canEditCollection) {
        ImGui::BeginDisabled();
    }
    if (contentActionButton("DeleteCollection", EditorGlyphIcon::Trash, "Delete Collection", "Delete this saved asset collection")) {
        const bool deletingActiveCollection = [&] {
            const EditorAssetCollection* selected = selectedCollection(state.editorPrefs, registryCollectionFilter_);
            return selected != nullptr && selected->name == collectionName;
        }();
        state.editorPrefs->removeAssetCollection(collectionName);
        if (deletingActiveCollection) {
            registryCollectionFilter_ = 0;
        }
        savePrefsStatus("Deleted collection: " + collectionName, "delete collection " + collectionName);
    }
    if (!canEditCollection) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(132.0f);
    ImGui::InputTextWithHint("##bulkTag", "Bulk add tag", bulkTagBuffer_.data(), bulkTagBuffer_.size());
    if (!tagSuggestions.empty()) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("##bulkTagPreset", "Tags")) {
            for (const std::string& tag : tagSuggestions) {
                if (ImGui::Selectable(tag.c_str(), false)) {
                    setTextBuffer(bulkTagBuffer_, tag);
                }
            }
            ImGui::EndCombo();
        }
    }
    const std::string bulkTag = trimString(bulkTagBuffer_.data());
    const bool bulkTagSaved = state.editorPrefs != nullptr && tagListContains(state.editorPrefs->assetTagPresets, bulkTag);
    ImGui::SameLine();
    if (state.editorPrefs == nullptr || bulkTag.empty() || bulkTagSaved) {
        ImGui::BeginDisabled();
    }
    if (contentActionButton("SaveBulkTagPreset", EditorGlyphIcon::Add, "Save Preset", "Save this tag as an editor preset")) {
        state.editorPrefs->addAssetTagPreset(bulkTag);
        savePrefsStatus("Saved tag preset: " + bulkTag, "save tag preset " + bulkTag);
    }
    if (state.editorPrefs == nullptr || bulkTag.empty() || bulkTagSaved) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (state.editorPrefs == nullptr || bulkTag.empty() || !bulkTagSaved) {
        ImGui::BeginDisabled();
    }
    if (contentActionButton("RemoveBulkTagPreset", EditorGlyphIcon::Trash, "Remove Preset", "Remove this tag from editor presets")) {
        state.editorPrefs->removeAssetTagPreset(bulkTag);
        savePrefsStatus("Removed tag preset: " + bulkTag, "remove tag preset " + bulkTag);
    }
    if (state.editorPrefs == nullptr || bulkTag.empty() || !bulkTagSaved) {
        ImGui::EndDisabled();
    }
    const bool canBulkTag = !bulkTag.empty() && !visibleRecordGuids.empty();
    ImGui::SameLine();
    if (!canBulkTag) {
        ImGui::BeginDisabled();
    }
    if (contentActionButton("BulkAddTagVisible", EditorGlyphIcon::Add, "Tag Visible", "Add this tag to all currently visible registry records")) {
        requests.bulkAddAssetTag = EditorBulkAssetTagRequest{visibleRecordGuids, bulkTag};
        status_ = "Queued bulk tag: " + bulkTag;
    }
    ImGui::SameLine();
    if (contentActionButton("BulkRemoveTagVisible", EditorGlyphIcon::Trash, "Untag Visible", "Remove this tag from all currently visible registry records")) {
        requests.bulkRemoveAssetTag = EditorBulkAssetTagRequest{visibleRecordGuids, bulkTag};
        status_ = "Queued bulk untag: " + bulkTag;
    }
    if (!canBulkTag) {
        ImGui::EndDisabled();
    }
    const std::filesystem::path sourceControlRoot = state.project != nullptr ? state.project->projectRoot : browserRoot_;
    auto cachedSourceControlStatus = [&](const std::filesystem::path& path) {
        if (path.empty()) {
            return std::string("Unavailable");
        }
        const std::string key = canonicalForCompare(path).string();
        auto it = sourceControlStatusCache_.find(key);
        if (it != sourceControlStatusCache_.end()) {
            return it->second;
        }
        const std::string status = gitStatusLabelForPath(sourceControlRoot, path);
        sourceControlStatusCache_[key] = status;
        return status;
    };
    auto summarizeRecordSourceControl = [&](const AssetRecord& record) {
        struct Candidate {
            const char* label = "";
            std::filesystem::path path;
        };
        const std::array<Candidate, 3> candidates = {{
            {"Src", resolveAssetRecordPath(state, record.sourcePath)},
            {"Meta", resolveAssetRecordPath(state, record.importedPath)},
            {"Payload", resolveAssetRecordPath(state, record.cachePath)},
        }};
        std::string fallback;
        for (const Candidate& candidate : candidates) {
            if (candidate.path.empty()) {
                continue;
            }
            const std::string status = cachedSourceControlStatus(candidate.path);
            if (sourceControlDiffReportAvailable(status)) {
                return std::string(candidate.label) + ": " + status;
            }
            if (fallback.empty()) {
                fallback = status;
            }
        }
        return fallback.empty() ? std::string("Unavailable") : fallback;
    };
    auto overwriteRisksForRecord = [&](const AssetRecord& record) {
        return collectAssetOverwriteRisks(state, record, cachedSourceControlStatus);
    };
    if (ImGui::BeginTable("AssetRegistryRecords", 13, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("Fav", ImGuiTableColumnFlags_WidthFixed, 38.0f);
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("GUID");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Imported");
        ImGui::TableSetupColumn("Git", ImGuiTableColumnFlags_WidthFixed, 118.0f);
        ImGui::TableSetupColumn("Tags");
        ImGui::TableSetupColumn("Deps");
        ImGui::TableSetupColumn("Refs");
        ImGui::TableSetupColumn("Missing/Stale");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::progressColumnWidth);
        ImGui::TableHeadersRow();
        for (const AssetRecord& record : records) {
            if (!recordMatchesFilters(record)) {
                continue;
            }
            ImGui::PushID(record.guid.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool favoriteAsset = state.editorPrefs != nullptr && assetGuidListContains(state.editorPrefs->favoriteAssetGuids, record.guid);
            ImGui::TextUnformatted(favoriteAsset ? "*" : "");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && favoriteAsset) {
                ImGui::SetTooltip("Favorite asset");
            }
            ImGui::TableSetColumnIndex(1);
            const ImVec2 typeCursor = ImGui::GetCursorScreenPos();
            const float typeIconY = typeCursor.y + std::max(0.0f, (EditorUiMetric::contentRowHeight - 16.0f) * 0.5f);
            drawAssetTypeGlyph(record.type, ImVec2(typeCursor.x, typeIconY), ImVec2(typeCursor.x + 16.0f, typeIconY + 16.0f));
            ImGui::Dummy(ImVec2(20.0f, EditorUiMetric::contentRowHeight));
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextUnformatted(assetTypeName(record.type));
            ImGui::TableSetColumnIndex(2);
            const char* name = record.displayName.empty() ? "(unnamed)" : record.displayName.c_str();
            editorPushRowSelectionStyle();
            if (ImGui::Selectable(name, selectedRecordGuid_ == record.guid, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, EditorUiMetric::contentRowHeight))) {
                selectedRecordGuid_ = record.guid;
                selectedPath_.clear();
            }
            editorPopRowSelectionStyle();
            if (record.type == AssetType::Prefab && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                std::array<char, 128> guidPayload{};
                record.guid.copy(guidPayload.data(), std::min(record.guid.size(), guidPayload.size() - 1));
                ImGui::SetDragDropPayload("PREFAB_ASSET", guidPayload.data(), guidPayload.size());
                ImGui::Text("Prefab %s", name);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (state.editorPrefs != nullptr) {
                    if (favoriteAsset) {
                        if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Remove Asset Favorite")) {
                            state.editorPrefs->removeFavoriteAsset(record.guid);
                            savePrefsStatus(
                                "Removed asset favorite: " + (record.displayName.empty() ? record.guid : record.displayName),
                                "remove asset favorite " + record.guid);
                        }
                    } else if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Add Asset Favorite")) {
                        state.editorPrefs->addFavoriteAsset(record.guid);
                        savePrefsStatus(
                            "Added asset favorite: " + (record.displayName.empty() ? record.guid : record.displayName),
                            "add asset favorite " + record.guid);
                    }
                    ImGui::Separator();
                }
                if (record.type == AssetType::Prefab && editorGlyphMenuItem(EditorGlyphIcon::Add, "Place Prefab")) {
                    requests.placeAsset = record.guid;
                }
                const std::filesystem::path resolvedSourcePath = resolveAssetRecordPath(state, record.sourcePath);
                const bool canReimport = !record.sourcePath.empty() && std::filesystem::exists(resolvedSourcePath);
                const std::vector<AssetOverwriteRisk> overwriteRisks = overwriteRisksForRecord(record);
                if (!overwriteRisks.empty()) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Reimport overwrite warning");
                    for (const AssetOverwriteRisk& risk : overwriteRisks) {
                        ImGui::BulletText("%s: %s", risk.label.c_str(), risk.status.c_str());
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("%s", risk.path.string().c_str());
                        }
                    }
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Refresh, "Reimport", canReimport)) {
                    requests.reimportAsset = record.guid;
                    recordImportOperation("Reimport Asset", resolvedSourcePath, {}, "Reimport", record.guid);
                    status_ = overwriteRisks.empty()
                        ? "Queued reimport: " + record.displayName
                        : "Queued reimport after overwrite warning: " + record.displayName;
                }
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(record.guid.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(record.sourcePath.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(record.importedPath.c_str());
            ImGui::TableSetColumnIndex(6);
            const std::string scmStatus = summarizeRecordSourceControl(record);
            ImGui::TextColored(sourceControlStatusTextColor(scmStatus.find(':') == std::string::npos ? scmStatus : trimString(scmStatus.substr(scmStatus.find(':') + 1))), "%s", scmStatus.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("First changed Git status among source, metadata, and payload paths");
            }
            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(joinTagList(record.tags).c_str());
            ImGui::TableSetColumnIndex(8);
            ImGui::Text("%zu", record.dependencies.size());
            ImGui::TableSetColumnIndex(9);
            ImGui::Text("%zu", record.references.size());
            ImGui::TableSetColumnIndex(10);
            ImGui::Text("%s%s", record.missing ? "missing" : "ok", record.stale ? " / stale" : "");
            ImGui::TableSetColumnIndex(11);
            ImGui::TextUnformatted(assetImportStatusName(record.status));
            ImGui::TableSetColumnIndex(12);
            ImGui::ProgressBar(assetImportProgress(record), ImVec2(-FLT_MIN, 0.0f), assetImportProgressLabel(record));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void AssetBrowserPanel::drawDetails(const EditorRuntimeState& state, EditorRequests& requests) {
    ImGui::SeparatorText("Details");
    const std::filesystem::path sourceControlRoot = state.project != nullptr ? state.project->projectRoot : browserRoot_;
    auto savePrefsStatus = [&](std::string successMessage, std::string failureDetail) {
        if (state.editorPrefs == nullptr) {
            return;
        }
        setPreferenceSaveStatus(state.editorPrefs->save(EditorPreferences::defaultPath()), status_, std::move(successMessage), std::move(failureDetail));
    };
    auto sourceControlStatus = [&](const std::filesystem::path& path) {
        if (path.empty()) {
            return std::string("Unavailable");
        }
        const std::string key = canonicalForCompare(path).string();
        auto it = sourceControlStatusCache_.find(key);
        if (it != sourceControlStatusCache_.end()) {
            return it->second;
        }
        const std::string status = gitStatusLabelForPath(sourceControlRoot, path);
        sourceControlStatusCache_[key] = status;
        return status;
    };
    auto drawSourceControlStatus = [&](const char* label, const std::filesystem::path& path) {
        if (path.empty()) {
            return;
        }
        const std::string status = sourceControlStatus(path);
        ImGui::TextColored(sourceControlStatusTextColor(status), "%s Source Control: %s", label, status.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", path.string().c_str());
        }
    };
    auto drawSourceControlActions = [&](const char* id, const std::filesystem::path& path) {
        if (path.empty()) {
            return;
        }
        const std::string status = sourceControlStatus(path);
        const bool canOpenExternal = regularFileExists(path);
        const bool canDiff = sourceControlDiffReportAvailable(status);
        ImGui::PushID(id);
        if (!canOpenExternal) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("OpenExternal", EditorGlyphIcon::File, "Open External", "Open this file with the OS-associated external tool")) {
            requests.openFilePath = path;
            status_ = "Opening file: " + path.string();
        }
        if (!canOpenExternal) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!canDiff) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("GitDiff", EditorGlyphIcon::Details, "Git Diff", "Write and open a Git diff report for this path")) {
            std::filesystem::path reportPath;
            std::string error;
            if (writeSourceControlDiffReport(state, browserRoot_, sourceControlRoot, path, reportPath, error)) {
                requests.openFilePath = reportPath;
                status_ = "Source-control diff report: " + reportPath.string();
            } else {
                status_ = "Source-control diff failed: " + error;
            }
        }
        if (!canDiff) {
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    };
    if (ImGui::SmallButton("Refresh Source Control")) {
        sourceControlStatusCache_.clear();
        clearGitStatusSnapshots();
        status_ = "Source control status refreshed";
    }
    if (!selectedPath_.empty()) {
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        const float previewWidth = std::min(ImGui::GetContentRegionAvail().x, EditorUiMetric::assetPreviewMaxWidth);
        const ImVec2 previewSize(previewWidth, EditorUiMetric::assetPreviewHeight);
        ImGui::InvisibleButton("AssetPreview", previewSize);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(20, 23, 27, 255), 4.0f);
        drawList->AddRect(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(55, 62, 72, 255), 4.0f);
        const ImVec2 previewMax(previewPos.x + previewSize.x, previewPos.y + previewSize.y);
        const bool hasGpuPreview = drawGpuSceneTextureThumbnail(state, selectedPath_, previewPos, previewMax);
        const bool hasStandaloneGpuPreview = hasGpuPreview ? false : drawStandaloneGpuAssetPreview(state, selectedPath_, previewPos, previewMax, false);
        const bool hasRasterPreview = (hasGpuPreview || hasStandaloneGpuPreview) ? false : drawRasterThumbnail(selectedPath_, previewPos, previewMax, false);
        if (!hasGpuPreview && !hasStandaloneGpuPreview && !hasRasterPreview) {
            const bool hasGeneratedPreview = drawGeneratedSourcePreview(selectedPath_, previewPos, previewMax);
            if (!hasGeneratedPreview) {
                const ImVec2 previewIconSize(34.0f, 34.0f);
                drawContentGlyph(
                    selectedPath_,
                    ImVec2(previewPos.x + previewSize.x * 0.5f - previewIconSize.x * 0.5f, previewPos.y + 28.0f),
                    ImVec2(previewPos.x + previewSize.x * 0.5f + previewIconSize.x * 0.5f, previewPos.y + 28.0f + previewIconSize.y));
                const std::string previewKind = contentKindLabel(selectedPath_);
                const ImVec2 kindSize = ImGui::CalcTextSize(previewKind.c_str());
                drawList->AddText(
                    ImVec2(previewPos.x + previewSize.x * 0.5f - kindSize.x * 0.5f, previewPos.y + 64.0f),
                    IM_COL32(130, 137, 148, 255),
                    previewKind.c_str());
            }
        }
        ImGui::Text("Selected: %s", selectedPath_.filename().string().c_str());
        ImGui::Text("Kind: %s", contentKindLabel(selectedPath_).c_str());
        ImGui::TextWrapped("Path: %s", relativeContentPath(selectedPath_).c_str());
        ImGui::SeparatorText("Selected Asset State");
        ImGui::Text("Origin: %s", selectedPathOriginLabel(state, selectedPath_));
        drawSourceControlStatus("Selected", selectedPath_);
        drawSourceControlActions("SelectedPathSourceControlActions", selectedPath_);
        if (state.assetRegistry != nullptr && state.assetRegistry->dirty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Registry Metadata: Unsaved changes");
        } else if (state.assetRegistry != nullptr) {
            ImGui::TextColored(ImVec4(0.54f, 0.82f, 0.60f, 1.0f), "Registry Metadata: Saved");
        } else {
            ImGui::TextDisabled("Registry Metadata: unavailable");
        }
        ImGui::TextDisabled("Imported asset state is exposed through registry records.");
        const bool isDirectory = std::filesystem::is_directory(selectedPath_);
        if (isDirectory) {
            if (contentActionButton("OpenFolder", EditorGlyphIcon::Folder, "Open Folder", "Open this folder in Content")) {
                navigateTo(selectedPath_);
            }
            ImGui::SameLine();
            if (compatibilityMode_) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ImportHere", EditorGlyphIcon::Import, "Import Here", "Import a model asset into this folder")) {
                if (auto source = openGltfFileDialog()) {
                    prepareImportDialog(*source, selectedPath_, 0);
                }
            }
            if (compatibilityMode_) {
                ImGui::EndDisabled();
            }
        } else {
            const bool canOpen = canOpenOrApplyPath(selectedPath_);
            if (!canOpen) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("OpenApply", EditorGlyphIcon::File, "Open / Apply", "Open the selected asset or apply it to the scene")) {
                loadFromPath(selectedPath_, requests);
            }
            if (!canOpen) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            const bool canImport = !compatibilityMode_ && isImportableSourceAssetPath(selectedPath_);
            const bool canImportAndPlace = !compatibilityMode_ && isPlaceablePrefabSourcePath(selectedPath_);
            if (!canImport) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ImportAsset", EditorGlyphIcon::Import, "Import Asset", "Import this source asset into the project asset registry")) {
                prepareImportDialog(selectedPath_, currentPath_, 0);
            }
            if (!canImport) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (!canImportAndPlace) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("PlaceAsset", EditorGlyphIcon::Add, "Place", "Import and place this model in the current scene")) {
                prepareImportDialog(selectedPath_, currentPath_, 1);
            }
            if (!canImportAndPlace) {
                ImGui::EndDisabled();
            }
        }
        if (contentActionButton("CopyPath", EditorGlyphIcon::Command, "Copy Path", "Copy the selected asset path to the clipboard")) {
            copyPathToClipboard(selectedPath_);
            status_ = "Copied path: " + selectedPath_.string();
        }
        ImGui::SameLine();
        if (contentActionButton("ShowInExplorer", EditorGlyphIcon::Folder, "Show in Explorer", "Reveal the selected asset in Explorer")) {
            revealPathInFileBrowser(selectedPath_);
        }
        if (!supportedContentPath(selectedPath_)) {
            ImGui::TextDisabled("No supported files selected");
        }
    } else if (selectedRecordGuid_.empty()) {
        ImGui::TextDisabled("No supported files selected");
    }
    const bool hasDetailsSelection = !selectedPath_.empty() || !selectedRecordGuid_.empty();
    if (!selectedRecordGuid_.empty() && state.assetRegistry != nullptr) {
        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (record.guid != selectedRecordGuid_) {
                continue;
            }
            const std::filesystem::path recordPreviewSource = recordPreviewPath(state, record);
            const ImVec2 previewPos = ImGui::GetCursorScreenPos();
            const float previewWidth = std::min(ImGui::GetContentRegionAvail().x, EditorUiMetric::assetPreviewMaxWidth);
            const ImVec2 previewSize(previewWidth, EditorUiMetric::assetPreviewHeight);
            ImGui::InvisibleButton("AssetRecordPreview", previewSize);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(20, 23, 27, 255), 4.0f);
            drawList->AddRect(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(55, 62, 72, 255), 4.0f);
            const ImVec2 previewMax(previewPos.x + previewSize.x, previewPos.y + previewSize.y);
            const bool previewDrawn = !recordPreviewSource.empty() &&
                (drawGpuSceneTextureThumbnail(state, recordPreviewSource, previewPos, previewMax) ||
                    drawStandaloneGpuAssetPreview(state, recordPreviewSource, previewPos, previewMax, false) ||
                    drawRasterThumbnail(recordPreviewSource, previewPos, previewMax, false) ||
                    drawGeneratedSourcePreview(recordPreviewSource, previewPos, previewMax));
            if (!previewDrawn) {
                const ImVec2 previewIconSize(34.0f, 34.0f);
                drawAssetTypeGlyph(
                    record.type,
                    ImVec2(previewPos.x + previewSize.x * 0.5f - previewIconSize.x * 0.5f, previewPos.y + 28.0f),
                    ImVec2(previewPos.x + previewSize.x * 0.5f + previewIconSize.x * 0.5f, previewPos.y + 28.0f + previewIconSize.y));
                const std::string previewKind = std::string(assetTypeName(record.type)) + " preview unavailable";
                const ImVec2 kindSize = ImGui::CalcTextSize(previewKind.c_str());
                drawList->AddText(
                    ImVec2(previewPos.x + previewSize.x * 0.5f - kindSize.x * 0.5f, previewPos.y + 64.0f),
                    IM_COL32(130, 137, 148, 255),
                    previewKind.c_str());
            }
            ImGui::Text("Asset: %s", record.displayName.empty() ? "(unnamed)" : record.displayName.c_str());
            ImGui::Text("GUID: %s", record.guid.c_str());
            ImGui::Text("Type: %s", assetTypeName(record.type));
            ImGui::SeparatorText("Selected Asset State");
            ImGui::TextColored(selectedAssetStateColor(record), "Asset State: %s", selectedAssetStateLabel(record));
            if (state.assetRegistry->dirty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Registry Metadata: Unsaved changes");
            } else {
                ImGui::TextColored(ImVec4(0.54f, 0.82f, 0.60f, 1.0f), "Registry Metadata: Saved");
            }
            if (state.editorPrefs != nullptr) {
                const bool favoriteAsset = assetGuidListContains(state.editorPrefs->favoriteAssetGuids, record.guid);
                if (favoriteAsset) {
                    if (contentActionButton("RemoveAssetFavorite", EditorGlyphIcon::Trash, "Remove Favorite", "Remove this asset GUID from editor favorites")) {
                        state.editorPrefs->removeFavoriteAsset(record.guid);
                        savePrefsStatus(
                            "Removed asset favorite: " + (record.displayName.empty() ? record.guid : record.displayName),
                            "remove asset favorite " + record.guid);
                    }
                } else if (contentActionButton("AddAssetFavorite", EditorGlyphIcon::Add, "Add Favorite", "Save this asset GUID as an editor favorite")) {
                    state.editorPrefs->addFavoriteAsset(record.guid);
                    savePrefsStatus(
                        "Added asset favorite: " + (record.displayName.empty() ? record.guid : record.displayName),
                        "add asset favorite " + record.guid);
                }
            }
            if (record.stale || record.status == AssetImportStatus::Stale) {
                ImGui::TextWrapped("The selected asset has stale import metadata; reimport updates its cooked payload and registry record.");
            } else if (record.missing || record.status == AssetImportStatus::Missing) {
                ImGui::TextWrapped("The selected asset has broken registry references. Reimport or repair missing metadata, cooked payload, or dependency records before placing it.");
            } else if (record.status == AssetImportStatus::Failed) {
                ImGui::TextWrapped("The selected asset import failed; reimport or inspect the source path before placing it.");
            } else if (record.sourceMissing) {
                ImGui::TextWrapped("The raw source path is missing, but imported metadata and cooked payload references are still available.");
            }
            if (record.sourceMissing) {
                ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Source file missing");
            }
            if (record.importedMetadataMissing) {
                ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "Imported metadata missing");
            }
            if (record.cookedPayloadMissing) {
                ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "Cooked payload missing");
            }
            if (record.dependenciesMissing) {
                ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "Dependency record missing");
            }
            const bool brokenPlaceholderRequired = record.missing || record.status == AssetImportStatus::Missing || record.sourceMissing || record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing;
            if (brokenPlaceholderRequired) {
                ImGui::TextWrapped("Broken placeholder state: placement and packaging should treat this record as broken until missing metadata, payload, source, or dependency records are repaired.");
            } else {
                ImGui::TextDisabled("Broken placeholder state: not required for the loaded registry health state.");
            }
            if (contentActionButton("BrokenPlaceholderReport", EditorGlyphIcon::Details, "Broken Placeholder", "Write and open a broken-asset placeholder readiness report")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetBrokenPlaceholderReport(state, browserRoot_, record, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset broken-placeholder report: " + reportPath.string();
                } else {
                    status_ = "Asset broken-placeholder report failed: " + error;
                }
            }
            ImGui::TextWrapped("Thumbnail: %s", record.thumbnailPath.empty() ? "(fallback icon)" : record.thumbnailPath.c_str());
            const std::filesystem::path resolvedThumbnailPath = resolveAssetRecordPath(state, record.thumbnailPath);
            if (!record.thumbnailPath.empty() && !regularFileExists(resolvedThumbnailPath)) {
                ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Thumbnail state: missing; using fallback preview");
            } else if (!record.thumbnailPath.empty()) {
                ImGui::TextColored(ImVec4(0.54f, 0.82f, 0.60f, 1.0f), "Thumbnail state: available");
            } else {
                ImGui::TextDisabled("Thumbnail state: no generated thumbnail metadata yet");
            }
            ImGui::TextWrapped("Source: %s", record.sourcePath.c_str());
            drawSourceControlStatus("Source", resolveAssetRecordPath(state, record.sourcePath));
            drawSourceControlActions("RecordSourceSourceControlActions", resolveAssetRecordPath(state, record.sourcePath));
            ImGui::TextWrapped("Imported: %s", record.importedPath.c_str());
            drawSourceControlStatus("Metadata", resolveAssetRecordPath(state, record.importedPath));
            drawSourceControlActions("RecordMetadataSourceControlActions", resolveAssetRecordPath(state, record.importedPath));
            ImGui::TextWrapped("Cache: %s", record.cachePath.c_str());
            drawSourceControlStatus("Payload", resolveAssetRecordPath(state, record.cachePath));
            drawSourceControlActions("RecordPayloadSourceControlActions", resolveAssetRecordPath(state, record.cachePath));
            if (assetTagsBufferGuid_ != record.guid) {
                setTextBuffer(assetTagsBuffer_, joinTagList(record.tags));
                assetTagsBufferGuid_ = record.guid;
            }
            ImGui::SeparatorText("Tags");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##assetTags", "comma-separated tags", assetTagsBuffer_.data(), assetTagsBuffer_.size());
            const std::vector<std::string> editedTags = parseTagList(assetTagsBuffer_.data());
            const bool tagsChanged = editedTags != parseTagList(joinTagList(record.tags));
            if (!tagsChanged) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ApplyTags", EditorGlyphIcon::Refresh, "Apply Tags", "Update this asset's registry tags")) {
                requests.updateAssetTags = EditorAssetTagsRequest{record.guid, editedTags};
                status_ = "Queued asset tag update: " + record.displayName;
            }
            if (!tagsChanged) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (record.tags.empty()) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ClearTags", EditorGlyphIcon::Trash, "Clear Tags", "Remove all tags from this asset")) {
                std::fill(assetTagsBuffer_.begin(), assetTagsBuffer_.end(), '\0');
                requests.updateAssetTags = EditorAssetTagsRequest{record.guid, {}};
                status_ = "Queued asset tag clear: " + record.displayName;
            }
            if (record.tags.empty()) {
                ImGui::EndDisabled();
            }
            const std::vector<std::string> registryTags = mergedTagSuggestions(collectRegistryTags(state.assetRegistry), state.editorPrefs);
            bool hasSuggestedTags = false;
            for (const std::string& tag : registryTags) {
                if (!tagListContains(record.tags, tag)) {
                    hasSuggestedTags = true;
                    break;
                }
            }
            if (hasSuggestedTags) {
                ImGui::TextDisabled("Existing tags");
                for (const std::string& tag : registryTags) {
                    if (tagListContains(record.tags, tag)) {
                        continue;
                    }
                    ImGui::PushID(tag.c_str());
                    if (contentActionButton("AddExistingTag", EditorGlyphIcon::Add, tag.c_str(), "Add this existing registry tag to the selected asset")) {
                        std::vector<std::string> updatedTags = record.tags;
                        updatedTags.push_back(tag);
                        requests.updateAssetTags = EditorAssetTagsRequest{record.guid, updatedTags};
                        setTextBuffer(assetTagsBuffer_, joinTagList(parseTagList(joinTagList(updatedTags))));
                        status_ = "Queued asset tag add: " + tag;
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }
            const std::filesystem::path resolvedSourceForReveal = resolveAssetRecordPath(state, record.sourcePath);
            const std::filesystem::path resolvedImportedForReveal = resolveAssetRecordPath(state, record.importedPath);
            const std::filesystem::path resolvedCacheForReveal = resolveAssetRecordPath(state, record.cachePath);
            const std::vector<AssetOverwriteRisk> overwriteRisks = collectAssetOverwriteRisks(state, record, sourceControlStatus);
            auto drawRevealAction = [&](const char* id, const char* label, const char* tooltip, const std::filesystem::path& path) {
                const bool canReveal = !path.empty() && std::filesystem::exists(path);
                if (!canReveal) {
                    ImGui::BeginDisabled();
                }
                if (contentActionButton(id, EditorGlyphIcon::Folder, label, tooltip)) {
                    revealPathInFileBrowser(path);
                    status_ = std::string("Revealed: ") + path.string();
                }
                if (!canReveal) {
                    ImGui::EndDisabled();
                }
            };
            ImGui::PushID("RecordRepairActions");
            if (contentActionButton("CopyGuid", EditorGlyphIcon::Command, "Copy GUID", "Copy this asset GUID to the clipboard")) {
                ImGui::SetClipboardText(record.guid.c_str());
                status_ = "Copied asset GUID: " + record.guid;
            }
            if (!record.sourcePath.empty()) {
                ImGui::SameLine();
                if (contentActionButton("CopySource", EditorGlyphIcon::Command, "Copy Source", "Copy the resolved source path to the clipboard")) {
                    copyPathToClipboard(resolvedSourceForReveal.empty() ? std::filesystem::path(record.sourcePath) : resolvedSourceForReveal);
                    status_ = "Copied source path: " + record.sourcePath;
                }
                ImGui::SameLine();
                drawRevealAction("RevealSource", "Reveal Source", "Reveal the source asset in Explorer", resolvedSourceForReveal);
            }
            if (!compatibilityMode_) {
                ImGui::SameLine();
                if (contentActionButton("RelinkSource", EditorGlyphIcon::Refresh, "Relink Source", "Choose a replacement raw source path for this asset record")) {
                    if (auto source = openImportAssetFileDialog()) {
                        requests.relinkAssetSource = EditorAssetRelinkSourceRequest{record.guid, *source};
                        status_ = "Queued source relink: " + record.displayName;
                    }
                }
            }
            if (!record.importedPath.empty()) {
                ImGui::SameLine();
                drawRevealAction("RevealImported", "Reveal Metadata", "Reveal the imported metadata file in Explorer", resolvedImportedForReveal);
            }
            if (!record.cachePath.empty()) {
                ImGui::SameLine();
                drawRevealAction("RevealCache", "Reveal Payload", "Reveal the cooked/runtime payload in Explorer", resolvedCacheForReveal);
            }
            if ((!record.sourcePath.empty() && !std::filesystem::exists(resolvedSourceForReveal)) ||
                (!record.importedPath.empty() && !std::filesystem::exists(resolvedImportedForReveal)) ||
                (!record.cachePath.empty() && !std::filesystem::exists(resolvedCacheForReveal))) {
                ImGui::TextDisabled("Missing paths can be repaired by restoring files or reimporting when the source is available.");
            }
            if (!overwriteRisks.empty()) {
                ImGui::SeparatorText("Source Control Overwrite Warning");
                ImGui::TextWrapped("Reimport or Rebuild Payload may overwrite generated asset files that have external source-control changes. Review the changed files before continuing.");
                for (const AssetOverwriteRisk& risk : overwriteRisks) {
                    ImGui::TextColored(sourceControlStatusTextColor(risk.status), "%s: %s", risk.label.c_str(), risk.status.c_str());
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", risk.path.string().c_str());
                    }
                }
                if (contentActionButton("OverwriteRiskReport", EditorGlyphIcon::Details, "Review Overwrite Risk", "Write and open a report of generated files that may be overwritten by reimport/rebuild")) {
                    std::filesystem::path reportPath;
                    std::string error;
                    if (writeAssetOverwriteRiskReport(state, browserRoot_, record, overwriteRisks, reportPath, error)) {
                        requests.openFilePath = reportPath;
                        status_ = "Asset overwrite-risk report: " + reportPath.string();
                    } else {
                        status_ = "Asset overwrite-risk report failed: " + error;
                    }
                }
            }
            if (contentActionButton("ValidateSelectedAsset", EditorGlyphIcon::Details, "Validate Asset", "Write a validation report for this asset and open it")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetValidationReport(state, browserRoot_, reportPath, error, record.guid)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset validation report: " + reportPath.string();
                } else {
                    status_ = "Asset validation failed: " + error;
                }
            }
            ImGui::SameLine();
            if (contentActionButton("ShowDependencies", EditorGlyphIcon::Details, "Show Dependencies", "Write and open a report of assets this record depends on")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetRelationshipReport(state, browserRoot_, record.guid, false, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset dependency report: " + reportPath.string();
                } else {
                    status_ = "Asset dependency report failed: " + error;
                }
            }
            ImGui::SameLine();
            if (contentActionButton("ShowReferences", EditorGlyphIcon::Details, "Show References", "Write and open a report of assets and scene components that reference this record")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetRelationshipReport(state, browserRoot_, record.guid, true, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset reference report: " + reportPath.string();
                } else {
                    status_ = "Asset reference report failed: " + error;
                }
            }
            const AssetUsageSummary usageSummary = assetUsageSummaryForRecord(state, record);
            ImGui::SeparatorText("Move / Delete Guard");
            if (usageSummary.referenced()) {
                ImGui::TextColored(
                    ImVec4(0.95f, 0.68f, 0.28f, 1.0f),
                    "Referenced by %zu registry link%s and %zu current-scene use%s",
                    usageSummary.registryReferences,
                    usageSummary.registryReferences == 1 ? "" : "s",
                    usageSummary.sceneReferences,
                    usageSummary.sceneReferences == 1 ? "" : "s");
                ImGui::TextWrapped("Inspect references or replace them before moving or deleting this asset. This live warning covers the loaded registry and current scene; Delete Readiness also runs the saved project metadata scan.");
            } else {
                ImGui::TextDisabled("No loaded registry or current-scene references found for this asset.");
            }
            if (contentActionButton("DeleteReadinessReport", EditorGlyphIcon::Details, "Delete Readiness", "Write and open a loaded-registry/current-scene/saved-project delete-readiness report")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetDeleteReadinessReport(state, browserRoot_, record.guid, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset delete-readiness report: " + reportPath.string();
                } else {
                    status_ = "Asset delete-readiness report failed: " + error;
                }
            }
            ImGui::SameLine();
            if (contentActionButton("ProjectReferenceScan", EditorGlyphIcon::Details, "Project References", "Scan saved project content and scene metadata for this asset GUID")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetProjectReferenceScanReport(state, browserRoot_, record.guid, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset project reference scan report: " + reportPath.string();
                } else {
                    status_ = "Asset project reference scan failed: " + error;
                }
            }
            ImGui::SeparatorText("Reference Repair");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##replaceReferenceGuid", "Replacement asset GUID", replaceReferenceGuid_.data(), replaceReferenceGuid_.size());
            const AssetRecord* replacementRecord = nullptr;
            const std::string replacementGuid = replaceReferenceGuid_.data();
            if (!replacementGuid.empty()) {
                for (const AssetRecord& candidate : state.assetRegistry->records()) {
                    if (candidate.guid == replacementGuid) {
                        replacementRecord = &candidate;
                        break;
                    }
                }
            }
            const bool canReplaceReferences = replacementRecord != nullptr && replacementRecord->guid != record.guid && replacementRecord->type == record.type;
            if (!canReplaceReferences) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ReplaceReferences", EditorGlyphIcon::Refresh, "Replace References", "Replace current-scene and registry references to this asset with the replacement GUID")) {
                requests.replaceAssetReferences = EditorReplaceAssetReferencesRequest{record.guid, replacementRecord->guid};
                status_ = "Queued reference replacement: " + record.guid + " -> " + replacementRecord->guid;
            }
            if (!canReplaceReferences) {
                ImGui::EndDisabled();
            }
            if (!replacementGuid.empty() && replacementRecord == nullptr) {
                ImGui::TextDisabled("Replacement GUID is not in the loaded asset registry.");
            } else if (replacementRecord != nullptr && replacementRecord->type != record.type) {
                ImGui::TextDisabled("Replacement asset type must match the selected asset type.");
            } else if (replacementRecord != nullptr && replacementRecord->guid == record.guid) {
                ImGui::TextDisabled("Replacement GUID must be different from the selected asset.");
            }
            ImGui::PopID();
            ImGui::Text("Dependencies: %zu", record.dependencies.size());
            ImGui::Text("References: %zu", record.references.size());
            auto findRecordByGuid = [&](const AssetGuid& guid) -> const AssetRecord* {
                for (const AssetRecord& candidate : state.assetRegistry->records()) {
                    if (candidate.guid == guid) {
                        return &candidate;
                    }
                }
                return nullptr;
            };
            auto drawLinkedAssetTable = [&](const char* label, const std::vector<AssetDependency>* dependencies, const std::vector<AssetGuid>* references) {
                const size_t count = dependencies != nullptr ? dependencies->size() : references != nullptr ? references->size() : 0;
                if (count == 0) {
                    return;
                }
                ImGui::SeparatorText(label);
                const std::string tableId = std::string(label) + "Table";
                if (ImGui::BeginTable(tableId.c_str(), 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Asset");
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                    ImGui::TableHeadersRow();
                    for (size_t index = 0; index < count; ++index) {
                        const AssetGuid guid = dependencies != nullptr ? (*dependencies)[index].guid : (*references)[index];
                        const std::string role = dependencies != nullptr ? (*dependencies)[index].kind : "reference";
                        const AssetRecord* linked = findRecordByGuid(guid);
                        const std::string rowId = std::string(label) + "_" + std::to_string(index);
                        ImGui::PushID(rowId.c_str());
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(role.empty() ? "asset" : role.c_str());
                        ImGui::TableSetColumnIndex(1);
                        const std::string display = linked != nullptr && !linked->displayName.empty() ? linked->displayName : guid;
                        if (linked != nullptr) {
                            if (ImGui::Selectable(display.c_str())) {
                                selectedRecordGuid_ = linked->guid;
                                selectedPath_.clear();
                                status_ = "Selected linked asset: " + display;
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("%s", linked->guid.c_str());
                            }
                        } else {
                            ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "%s", guid.c_str());
                        }
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(linked != nullptr ? assetTypeName(linked->type) : "Missing");
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(linked != nullptr ? assetImportStatusName(linked->status) : "Broken");
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            };
            drawLinkedAssetTable("Dependencies", &record.dependencies, nullptr);
            drawLinkedAssetTable("References", nullptr, &record.references);
            auto drawReverseRegistryReferences = [&] {
                struct ReverseReferenceRow {
                    const AssetRecord* owner = nullptr;
                    std::string role;
                    std::string source;
                };
                std::vector<ReverseReferenceRow> rows;
                for (const AssetRecord& candidate : state.assetRegistry->records()) {
                    if (candidate.guid == record.guid) {
                        continue;
                    }
                    for (const AssetDependency& dependency : candidate.dependencies) {
                        if (dependency.guid == record.guid) {
                            rows.push_back(ReverseReferenceRow{&candidate, dependency.kind.empty() ? "dependency" : dependency.kind, "Dependency"});
                        }
                    }
                    for (const AssetGuid& reference : candidate.references) {
                        if (reference == record.guid) {
                            rows.push_back(ReverseReferenceRow{&candidate, "reference", "Reference"});
                        }
                    }
                }
                if (rows.empty()) {
                    return;
                }
                ImGui::SeparatorText("Used By Assets");
                if (ImGui::BeginTable("ReverseAssetReferencesTable", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Asset");
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                    ImGui::TableHeadersRow();
                    for (size_t index = 0; index < rows.size(); ++index) {
                        const ReverseReferenceRow& row = rows[index];
                        if (row.owner == nullptr) {
                            continue;
                        }
                        ImGui::PushID(static_cast<int>(index));
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(row.role.c_str());
                        ImGui::TableSetColumnIndex(1);
                        const std::string display = row.owner->displayName.empty() ? row.owner->guid : row.owner->displayName;
                        if (ImGui::Selectable(display.c_str())) {
                            selectedRecordGuid_ = row.owner->guid;
                            selectedPath_.clear();
                            status_ = "Selected referring asset: " + display;
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("%s", row.owner->guid.c_str());
                        }
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(assetTypeName(row.owner->type));
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(row.source.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            };
            auto drawSceneReferences = [&] {
                if (state.sceneDocument == nullptr) {
                    return;
                }
                struct SceneReferenceRow {
                    std::string entityName;
                    uint64_t entityUuid = 0;
                    std::string component;
                    std::string field;
                };
                std::vector<SceneReferenceRow> rows;
                for (const Entity* entity : state.sceneDocument->registry().entities()) {
                    if (entity == nullptr || !entity->meshRenderer.has_value()) {
                        continue;
                    }
                    const MeshRenderer& renderer = *entity->meshRenderer;
                    if (renderer.meshGuid == record.guid) {
                        rows.push_back(SceneReferenceRow{entity->name, entity->uuid, "MeshRenderer", "meshGuid"});
                    }
                    for (const MaterialSlot& slot : renderer.materialSlots) {
                        if (slot.materialGuid == record.guid) {
                            rows.push_back(SceneReferenceRow{entity->name, entity->uuid, "MeshRenderer", "materialGuid"});
                        }
                        if (slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == record.guid) {
                            rows.push_back(SceneReferenceRow{entity->name, entity->uuid, "MeshRenderer", "overrideMaterialGuid"});
                        }
                    }
                }
                for (const PrefabInstance& instance : state.sceneDocument->prefabInstances()) {
                    if (instance.prefabGuid == record.guid) {
                        rows.push_back(SceneReferenceRow{"Prefab Instance", instance.instanceRoot.index, "PrefabInstance", "prefabGuid"});
                    }
                }
                if (rows.empty()) {
                    return;
                }
                ImGui::SeparatorText("Used By Current Scene");
                if (ImGui::BeginTable("SceneAssetReferencesTable", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Entity");
                    ImGui::TableSetupColumn("UUID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthFixed, 112.0f);
                    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 132.0f);
                    ImGui::TableHeadersRow();
                    for (size_t index = 0; index < rows.size(); ++index) {
                        const SceneReferenceRow& row = rows[index];
                        ImGui::PushID(static_cast<int>(index));
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(row.entityName.empty() ? "(unnamed)" : row.entityName.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%llu", static_cast<unsigned long long>(row.entityUuid));
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(row.component.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(row.field.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            };
            drawReverseRegistryReferences();
            drawSceneReferences();
            ImGui::Text("Status: %s%s%s%s", assetImportStatusName(record.status), record.missing ? " missing" : "", record.stale ? " stale" : "", record.sourceMissing ? " source-missing" : "");
            ImGui::ProgressBar(assetImportProgress(record), ImVec2(-FLT_MIN, 0.0f), assetImportProgressLabel(record));
            const std::filesystem::path resolvedSourcePath = resolveAssetRecordPath(state, record.sourcePath);
            const bool canReimport = !record.sourcePath.empty() && !record.sourceMissing && std::filesystem::exists(resolvedSourcePath);
            const bool canRebuildPayload = canReimport && record.cookedPayloadMissing;
            if (!canRebuildPayload) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("RebuildPayload", EditorGlyphIcon::Refresh, "Rebuild Payload", "Queue a reimport to regenerate the missing cooked/runtime payload")) {
                requests.reimportAsset = record.guid;
                recordImportOperation("Rebuild Payload", resolvedSourcePath, {}, "Reimport", record.guid);
                status_ = overwriteRisks.empty()
                    ? "Queued payload rebuild: " + record.displayName
                    : "Queued payload rebuild after overwrite warning: " + record.displayName;
            }
            if (!canRebuildPayload) {
                ImGui::EndDisabled();
                if (record.cookedPayloadMissing && !canReimport) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Source unavailable");
                }
            }
            ImGui::SameLine();
            if (!canReimport) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ReimportRecord", EditorGlyphIcon::Refresh, "Reimport", "Queue this asset for reimport")) {
                requests.reimportAsset = record.guid;
                recordImportOperation("Reimport Asset", resolvedSourcePath, {}, "Reimport", record.guid);
                status_ = overwriteRisks.empty()
                    ? "Queued reimport: " + record.displayName
                    : "Queued reimport after overwrite warning: " + record.displayName;
            }
            if (!canReimport) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("Source unavailable");
            }
            if (record.type == AssetType::Prefab) {
                ImGui::SameLine();
                if (contentActionButton("PlacePrefab", EditorGlyphIcon::Add, "Place Prefab", "Place this prefab in the current scene")) {
                    requests.placeAsset = record.guid;
                }
            }
            break;
        }
    }
    if (hasDetailsSelection) {
        ImGui::SeparatorText("Context");
        if (state.project != nullptr) {
            ImGui::Text("Project: %s", state.project->name.c_str());
            ImGui::TextWrapped("Content Root: %s", state.project->contentRoot.string().c_str());
        } else {
            ImGui::TextDisabled("Project: none");
        }
        ImGui::Text("Current Folder: %s", currentPath_.empty() ? "(none)" : relativeContentPath(currentPath_).c_str());
    }
}

void AssetBrowserPanel::drawImportSettingsDialog(EditorRequests& requests) {
    if (openImportSettings_) {
        ImGui::OpenPopup("Import Settings");
        openImportSettings_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 430.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const char* modes[] = {"Import Asset", "Import and Place"};
    ImGui::Combo("Mode", &importMode_, modes, IM_ARRAYSIZE(modes));
    ImGui::InputText("Source", importSourcePath_.data(), importSourcePath_.size(), ImGuiInputTextFlags_ReadOnly);
    ImGui::InputTextWithHint("Destination Folder", "Models", importDestinationFolder_.data(), importDestinationFolder_.size());
    ImGui::SeparatorText("Source");
    ImGui::Checkbox("Copy source into project", &importSettings_.copySourceIntoProject);
    ImGui::SeparatorText("Hierarchy");
    ImGui::Checkbox("Preserve hierarchy", &importSettings_.preserveHierarchy);
    ImGui::Checkbox("Import materials", &importSettings_.importMaterials);
    ImGui::Checkbox("Import textures", &importSettings_.importTextures);
    ImGui::Checkbox("Import cameras", &importSettings_.importCameras);
    ImGui::Checkbox("Import lights", &importSettings_.importLights);
    ImGui::SeparatorText("Geometry / Cache");
    ImGui::Checkbox("Generate tangents", &importSettings_.generateTangents);
    ImGui::Checkbox("Build BLAS cache", &importSettings_.buildBlasCache);
    ImGui::InputFloat("Unit scale", &importSettings_.unitScale, 0.1f, 1.0f, "%.3f");
    static int coordinateMode = 0;
    const char* coordinateModes[] = {"None", "glTF Y-Up to Engine", "Z-Up to Engine"};
    if (ImGui::Combo("Coordinate conversion", &coordinateMode, coordinateModes, IM_ARRAYSIZE(coordinateModes))) {
        importSettings_.coordinateConversion = coordinateModes[coordinateMode];
    }

    if (ImGui::Button("Import")) {
        EditorImportAssetRequest request;
        request.sourcePath = std::filesystem::path(importSourcePath_.data());
        request.destinationFolder = std::filesystem::path(importDestinationFolder_.data());
        request.mode = importMode_ == 0 ? "ImportAsset" : "ImportAndPlace";
        request.settings = importSettings_;
        if (importMode_ == 0) {
            requests.importAsset = std::move(request);
            recordImportOperation("Import Asset", requests.importAsset->sourcePath, requests.importAsset->destinationFolder, requests.importAsset->mode);
            status_ = "Queued non-mutating Import Asset: " + requests.importAsset->sourcePath.string();
        } else {
            recordImportOperation("Import and Place", request.sourcePath, request.destinationFolder, request.mode);
            const std::filesystem::path sourcePath = request.sourcePath;
            requests.importAndPlace = std::move(request);
            status_ = "Queued Import and Place: " + sourcePath.string();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        status_ = "Import Asset cancelled";
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    (void)selection;
    if (!ImGui::Begin(EditorDockWindowTitle::Content)) {
        ImGui::End();
        return;
    }

    syncBrowserRoot(state);
    refreshImportOperations(state);

    ImGui::BeginGroup();
    if (editorIconButton("ContentAdd", EditorGlyphIcon::Add, false)) {
        ImGui::OpenPopup("ContentAddMenu");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Add or import content");
    }
    if (ImGui::BeginPopup("ContentAddMenu")) {
        ImGui::MenuItem("New Folder", nullptr, false, false);
        if (ImGui::MenuItem("New Scene", nullptr, false, true)) {
            requests.newScene = true;
        }
        ImGui::MenuItem("New Material", nullptr, false, false);
        ImGui::Separator();
        const bool canImportAssets = !compatibilityMode_;
        if (ImGui::MenuItem("Import Asset...", nullptr, false, canImportAssets)) {
            if (auto path = openImportAssetFileDialog()) {
                prepareImportDialog(*path, {}, 0);
            }
        }
        if (ImGui::MenuItem("Import Into Scene...", nullptr, false, canImportAssets)) {
            if (auto path = openGltfFileDialog()) {
                prepareImportDialog(*path, {}, 1);
            }
        }
        if (ImGui::MenuItem("Import Texture...", nullptr, false, canImportAssets)) {
            if (auto path = openTextureFileDialog()) {
                prepareImportDialog(*path, {}, 0);
            }
        }
        if (ImGui::MenuItem("Import HDRI...", nullptr, false, canImportAssets)) {
            if (auto path = openHdrFileDialog()) {
                prepareImportDialog(*path, {}, 0);
            }
        }
        ImGui::MenuItem("Import IES Profile...", nullptr, false, false);
        ImGui::MenuItem("Browse Filesystem...", nullptr, false, false);
        if (compatibilityMode_) {
            ImGui::Separator();
            if (ImGui::MenuItem("Open Project Manager")) {
                requests.showProjectManager = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool canValidateProject = state.assetRegistry != nullptr;
    if (!canValidateProject) {
        ImGui::BeginDisabled();
    }
    if (contentActionButton("ValidateProject", EditorGlyphIcon::Details, "Validate Project", "Write a project asset validation report and open it")) {
        std::filesystem::path reportPath;
        std::string error;
        if (writeAssetValidationReport(state, browserRoot_, reportPath, error)) {
            requests.openFilePath = reportPath;
            status_ = "Project validation report: " + reportPath.string();
        } else {
            status_ = "Project validation failed: " + error;
        }
    }
    if (!canValidateProject) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##contentFilter", "Filter in selected folder...", search_.data(), search_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(backStack_.empty());
    if (editorIconButton("ContentBack", EditorGlyphIcon::Back, false)) {
        forwardStack_.push_back(currentPath_);
        const std::filesystem::path previous = backStack_.back();
        backStack_.pop_back();
        navigateTo(previous, false);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Back");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(forwardStack_.empty());
    if (editorIconButton("ContentForward", EditorGlyphIcon::Forward, false)) {
        backStack_.push_back(currentPath_);
        const std::filesystem::path next = forwardStack_.back();
        forwardStack_.pop_back();
        navigateTo(next, false);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Forward");
    }
    showDetails_ = true;

    if (!browserRoot_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::PushID("ContentBreadcrumb");
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(currentPath_, browserRoot_, relativeError);
        const std::string rootLabel = compatibilityMode_ ? compactPathLabel(browserRoot_, "Workspace") : "Project";
        if (ImGui::SmallButton(rootLabel.c_str())) {
            navigateTo(browserRoot_);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", browserRoot_.string().c_str());
        }
        std::filesystem::path accum = browserRoot_;
        for (const auto& part : relativeError ? std::filesystem::path{} : relative) {
            const std::string partString = part.string();
            if (partString == "." || partString.empty()) {
                continue;
            }
            accum /= part;
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
            ImGui::PushID(partString.c_str());
            if (ImGui::SmallButton(partString.c_str())) {
                navigateTo(accum);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndGroup();
    drawImportSettingsDialog(requests);

    const std::string sceneLoadStatus = state.sceneLoadingStatus != nullptr ? *state.sceneLoadingStatus : std::string{};
    const bool hasSceneLoadStatus = !sceneLoadStatus.empty();
    const bool sceneLoadCompleted = !state.sceneLoadRunning && hasSceneLoadStatus && sceneLoadStatusIsSuccessfulCompletion(sceneLoadStatus);
    const bool showSceneLoadBanner = state.sceneLoadRunning || (hasSceneLoadStatus && !sceneLoadCompleted);
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status_.c_str());
    }
    if (showSceneLoadBanner) {
        const float progress = std::clamp(state.sceneLoadProgress, 0.0f, 1.0f);
        ImGui::ProgressBar(progress, ImVec2(std::min(360.0f, ImGui::GetContentRegionAvail().x), 0.0f));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.sceneLoadRunning ? "Import / load in progress" : "Last import / load status");
        ImGui::TextWrapped("%s", sceneLoadStatus.c_str());
    }

    const float browserHeight = ImGui::GetContentRegionAvail().y;
    if (browserHeight > ImGui::GetTextLineHeightWithSpacing()) {
        const float browserWidth = ImGui::GetContentRegionAvail().x;
        const float sectionSpacing = ImGui::GetStyle().ItemSpacing.x;
        const float treeWidth = std::clamp(
            browserWidth * EditorUiMetric::contentTreePanelRatio,
            EditorUiMetric::contentTreeMinWidth,
            EditorUiMetric::contentTreeMaxWidth);
        float detailsWidth = showDetails_
            ? std::clamp(
                browserWidth * EditorUiMetric::contentDetailsPanelRatio,
                EditorUiMetric::contentDetailsMinWidth,
                EditorUiMetric::contentDetailsMaxWidth)
            : 0.0f;
        if (showDetails_) {
            const float maxDetailsWidth = browserWidth - treeWidth - EditorUiMetric::contentListMinWidth - (sectionSpacing * 2.0f);
            detailsWidth = std::max(0.0f, std::min(detailsWidth, maxDetailsWidth));
        }
        ImGui::BeginChild("ContentFolders", ImVec2(treeWidth, 0.0f), true);
        if (!browserRoot_.empty()) {
            drawFolderTree(browserRoot_, requests);
        }
        if (state.editorPrefs != nullptr) {
            auto& prefs = *state.editorPrefs;
            auto savePrefsStatus = [&](std::string successMessage, std::string failureDetail) {
                setPreferenceSaveStatus(prefs.save(EditorPreferences::defaultPath()), status_, std::move(successMessage), std::move(failureDetail));
            };
            auto drawStoredPathEntry = [&](const char* listName, size_t index, const std::filesystem::path& path, bool favoriteRow) {
                std::error_code ec;
                const bool exists = std::filesystem::exists(path, ec);
                const bool directory = exists && std::filesystem::is_directory(path, ec);
                const bool selected = !selectedPath_.empty() && canonicalForCompare(selectedPath_) == canonicalForCompare(path);
                const std::string filename = path.filename().empty() ? path.string() : path.filename().string();
                const std::string label = editorGlyphLabel((exists ? filename : filename + " (missing)").c_str());
                ImGui::PushID(listName);
                ImGui::PushID(static_cast<int>(index));
                if (!exists) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                }
                if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedPath_ = path;
                    selectedRecordGuid_.clear();
                    if (exists && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (directory) {
                            navigateTo(path);
                        } else {
                            loadFromPath(path, requests);
                        }
                    }
                }
                if (!exists) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", path.string().c_str());
                }
                if (ImGui::BeginPopupContextItem("StoredPathContext")) {
                    if (directory) {
                        if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Open Folder", exists)) {
                            navigateTo(path);
                        }
                    } else if (editorGlyphMenuItem(editorGlyphForPath(path), "Open / Apply", exists && canOpenOrApplyPath(path))) {
                        loadFromPath(path, requests);
                    }
                    if (!favoriteRow && editorGlyphMenuItem(EditorGlyphIcon::Add, "Add to Favorites", exists && !preferencePathListContains(prefs.favoriteFiles, path))) {
                        prefs.addFavorite(path);
                        savePrefsStatus("Added favorite: " + path.string(), "add favorite " + path.string());
                    }
                    if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy Path")) {
                        copyPathToClipboard(path);
                        status_ = "Copied path: " + path.string();
                    }
                    if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Show in Explorer", exists)) {
                        revealPathInFileBrowser(path);
                    }
                    if (favoriteRow && editorGlyphMenuItem(EditorGlyphIcon::Trash, "Remove from Favorites")) {
                        prefs.removeFavorite(path.string());
                        savePrefsStatus("Removed favorite: " + path.string(), "remove favorite " + path.string());
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::PopID();
            };
            if (state.assetRegistry != nullptr && !prefs.favoriteAssetGuids.empty() && ImGui::TreeNodeEx("Favorite Assets", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::string assetFavoriteToRemove;
                for (size_t i = 0; i < prefs.favoriteAssetGuids.size(); ++i) {
                    const AssetGuid& guid = prefs.favoriteAssetGuids[i];
                    const AssetRecord* record = findAssetRecordByGuid(*state.assetRegistry, guid);
                    const bool missingRecord = record == nullptr;
                    const std::string displayName = missingRecord
                        ? guid + " (missing)"
                        : (record->displayName.empty() ? record->guid : record->displayName);
                    const std::string label = editorGlyphLabel(displayName);
                    ImGui::PushID("FavoriteAsset");
                    ImGui::PushID(static_cast<int>(i));
                    if (missingRecord) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                    }
                    if (ImGui::Selectable(label.c_str(), selectedRecordGuid_ == guid)) {
                        selectedRecordGuid_ = guid;
                        selectedPath_.clear();
                    }
                    if (missingRecord) {
                        ImGui::PopStyleColor();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", guid.c_str());
                    }
                    if (ImGui::BeginPopupContextItem("FavoriteAssetContext")) {
                        if (editorGlyphMenuItem(EditorGlyphIcon::Details, "Open Details", !missingRecord)) {
                            selectedRecordGuid_ = guid;
                            selectedPath_.clear();
                        }
                        if (!missingRecord && record->type == AssetType::Prefab && editorGlyphMenuItem(EditorGlyphIcon::Add, "Place Prefab")) {
                            requests.placeAsset = guid;
                        }
                        if (editorGlyphMenuItem(EditorGlyphIcon::Details, "Filter Registry To Favorites")) {
                            registryFavoriteFilter_ = 1;
                        }
                        if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy GUID")) {
                            ImGui::SetClipboardText(guid.c_str());
                            status_ = "Copied asset GUID: " + guid;
                        }
                        if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Remove Asset Favorite")) {
                            assetFavoriteToRemove = guid;
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                }
                if (!assetFavoriteToRemove.empty()) {
                    prefs.removeFavoriteAsset(assetFavoriteToRemove);
                    savePrefsStatus("Removed asset favorite: " + assetFavoriteToRemove, "remove asset favorite " + assetFavoriteToRemove);
                }
                ImGui::TreePop();
            }
            if (!prefs.favoriteFiles.empty() && ImGui::TreeNodeEx("Favorites", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < prefs.favoriteFiles.size(); ++i) {
                    drawStoredPathEntry("Favorite", i, std::filesystem::path(prefs.favoriteFiles[i]), true);
                }
                ImGui::TreePop();
            }
            if (!prefs.recentFiles.empty() && ImGui::TreeNodeEx("Recent", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < prefs.recentFiles.size(); ++i) {
                    drawStoredPathEntry("Recent", i, std::filesystem::path(prefs.recentFiles[i]), false);
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("ContentItems", ImVec2(-(detailsWidth + (showDetails_ ? sectionSpacing : 0.0f)), 0.0f), true);
        drawPathList(state, requests);
        drawRegistryTable(state, requests);
        drawImportOperations();
        ImGui::EndChild();
        if (showDetails_) {
            ImGui::SameLine();
            ImGui::BeginChild("ContentDetails", ImVec2(detailsWidth, 0.0f), true);
            drawDetails(state, requests);
            if (state.editorPrefs != nullptr && !selectedPath_.empty()) {
                const std::string storedFavorite = matchingPreferencePathValue(state.editorPrefs->favoriteFiles, selectedPath_);
                if (!storedFavorite.empty()) {
                    if (ImGui::SmallButton("Remove Selected Favorite")) {
                        state.editorPrefs->removeFavorite(storedFavorite);
                        setPreferenceSaveStatus(
                            state.editorPrefs->save(EditorPreferences::defaultPath()),
                            status_,
                            "Removed favorite: " + selectedPath_.string(),
                            "remove favorite " + selectedPath_.string());
                    }
                } else if (ImGui::SmallButton("Add Selected to Favorites")) {
                    state.editorPrefs->addFavorite(selectedPath_);
                    setPreferenceSaveStatus(
                        state.editorPrefs->save(EditorPreferences::defaultPath()),
                        status_,
                        "Added favorite: " + selectedPath_.string(),
                        "add favorite " + selectedPath_.string());
                }
            }
            ImGui::EndChild();
        }
    }

    ImGui::End();
}

} // namespace rtv
