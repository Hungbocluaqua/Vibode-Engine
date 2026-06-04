#include "rtv/EditorLayer.h"

#include "rtv/AssetImport.h"
#include "rtv/AssetManager.h"
#include "rtv/EditorCommands.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/Entity.h"
#include "rtv/FileDialog.h"
#include "rtv/SceneRenderSettingsSync.h"
#include "rtv/UndoStack.h"

#include <imgui.h>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <Shellapi.h>
#endif

namespace rtv {

namespace {

template <size_t N>
void copyTextToBuffer(std::array<char, N>& buffer, const std::string& value) {
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::memcpy(buffer.data(), value.data(), std::min(value.size(), buffer.size() - 1));
}

std::filesystem::path defaultVibodeProjectRoot() {
#if defined(_WIN32)
    char* userProfile = nullptr;
    size_t userProfileLength = 0;
    if (_dupenv_s(&userProfile, &userProfileLength, "USERPROFILE") == 0 && userProfile != nullptr) {
        std::filesystem::path root = std::filesystem::path(userProfile) / "Documents" / "Vibode Projects";
        std::free(userProfile);
        return root;
    }
#endif
    return std::filesystem::current_path() / "Vibode Projects";
}

bool hasInvalidWindowsPathCharacter(const std::string& value) {
    return value.find_first_of("<>:\"/\\|?*") != std::string::npos;
}

std::filesystem::path nearestExistingParent(std::filesystem::path path) {
    std::error_code ec;
    while (!path.empty() && !std::filesystem::exists(path, ec)) {
        path = path.parent_path();
    }
    return path;
}

bool pathLooksWritable(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        return false;
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    const std::filesystem::perms permissions = std::filesystem::status(path, ec).permissions();
    if (ec) {
        return false;
    }
    return (permissions & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
#endif
}

bool requestRendererCommand(EditorCommandId id, const EditorRuntimeState& state, EditorRequests& requests) {
    RendererSettings settings = state.renderer.settings();
    bool changed = true;
    switch (id) {
    case EditorCommandId::SetDebugBeauty:
        settings.debugView = RendererDebugView::Beauty;
        break;
    case EditorCommandId::SetDebugDirectLighting:
        settings.debugView = RendererDebugView::DirectLighting;
        break;
    case EditorCommandId::SetDebugIndirectLighting:
        settings.debugView = RendererDebugView::IndirectLighting;
        break;
    case EditorCommandId::SetDebugNormals:
        settings.debugView = RendererDebugView::Normals;
        break;
    case EditorCommandId::SetDebugDepth:
        settings.debugView = RendererDebugView::Depth;
        break;
    case EditorCommandId::SetDebugMotionVectors:
        settings.debugView = RendererDebugView::MotionVectors;
        break;
    case EditorCommandId::SetDebugVariance:
        settings.debugView = RendererDebugView::Variance;
        break;
    case EditorCommandId::SetDebugAlbedo:
        settings.debugView = RendererDebugView::Albedo;
        break;
    case EditorCommandId::ToggleMovingDenoiser:
        settings.denoiseWhileMoving = !settings.denoiseWhileMoving;
        break;
    case EditorCommandId::ToggleEnvironment:
        settings.environmentEnabled = !settings.environmentEnabled;
        break;
    case EditorCommandId::ToggleDirectLighting:
        settings.directLightingEnabled = !settings.directLightingEnabled;
        break;
    case EditorCommandId::SetToneMapperLinear:
        settings.toneMapper = ToneMapper::Linear;
        break;
    case EditorCommandId::SetToneMapperReinhard:
        settings.toneMapper = ToneMapper::Reinhard;
        break;
    case EditorCommandId::SetToneMapperAces:
        settings.toneMapper = ToneMapper::ACES;
        break;
    case EditorCommandId::SetToneMapperPbrNeutral:
        settings.toneMapper = ToneMapper::PBRNeutral;
        break;
    case EditorCommandId::SetToneMapperAgx:
        settings.toneMapper = ToneMapper::AgX;
        break;
    case EditorCommandId::ToggleAutoExposure:
        settings.autoExposureEnabled = !settings.autoExposureEnabled;
        break;
    default:
        changed = false;
        break;
    }
    if (!changed) {
        return false;
    }
    requestSettings(requests, settings);
    return true;
}

bool isViewportPanelCommand(EditorCommandId id) {
    switch (id) {
    case EditorCommandId::ViewportSelect:
    case EditorCommandId::ViewportMove:
    case EditorCommandId::ViewportRotate:
    case EditorCommandId::ViewportScale:
    case EditorCommandId::ViewportToggleLocal:
    case EditorCommandId::ViewportToggleSnap:
    case EditorCommandId::ViewportToggleGrid:
    case EditorCommandId::ViewportToggleAxes:
        return true;
    default:
        return false;
    }
}

uint32_t materialIdForCommandSelection(const EditorRuntimeState& state, const EditorSelection& selection) {
    const EditorSelectionId selected = selection.current();
    if (selected.kind == EditorSelectionKind::Material) {
        return selected.index;
    }
    if (state.sceneDocument != nullptr && selected.entity.valid()) {
        const Entity* entity = state.sceneDocument->registry().entity(selected.entity);
        if (entity == nullptr || !entity->meshRenderer.has_value()) {
            return UINT32_MAX;
        }
        const MeshRenderer& renderer = *entity->meshRenderer;
        if (!renderer.materialSlots.empty()) {
            const MaterialAssetHandle material = renderer.materialSlots.front().resolvedMaterial();
            return material.valid() ? material.index : UINT32_MAX;
        }
        if (state.assets != nullptr) {
            const MeshAsset* mesh = state.assets->mesh(renderer.mesh);
            if (mesh != nullptr && !mesh->primitives.empty()) {
                return mesh->primitives.front().material.index;
            }
        }
        return UINT32_MAX;
    }
    if (selected.kind != EditorSelectionKind::Object || state.importedScene == nullptr || state.assets == nullptr) {
        return UINT32_MAX;
    }
    if (selected.index >= state.importedScene->nodes.size()) {
        return UINT32_MAX;
    }
    const SceneNodeAsset& node = state.importedScene->nodes[selected.index];
    const MeshAsset* mesh = state.assets->mesh(node.mesh);
    if (mesh == nullptr || mesh->primitives.empty()) {
        return UINT32_MAX;
    }
    return mesh->primitives.front().material.index;
}

const AssetRecord* materialAssetRecordForLoadedMaterialCommand(const EditorRuntimeState& state, uint32_t materialId) {
    if (state.assetRegistry == nullptr || state.importedScene == nullptr) {
        return nullptr;
    }
    const auto& sceneMaterials = state.importedScene->materials;
    for (const AssetRecord& record : state.assetRegistry->records()) {
        if (record.type != AssetType::Material || record.sourceHash.empty() || record.importSettingsHash.empty()) {
            continue;
        }
        for (size_t i = 0; i < sceneMaterials.size(); ++i) {
            const MaterialAssetHandle handle = sceneMaterials[i];
            if (!handle.valid() || handle.index != materialId) {
                continue;
            }
            if (importedAssetGuidFor(record.sourceHash, record.importSettingsHash, "Material", i) == record.guid) {
                return &record;
            }
        }
    }
    return nullptr;
}

std::optional<AssetGuid> selectedDirtyMaterialAssetGuid(const EditorRuntimeState& state, const EditorSelection& selection) {
    if (state.dirtyMaterialAssets == nullptr || state.dirtyMaterialAssets->empty()) {
        return std::nullopt;
    }
    const uint32_t materialId = materialIdForCommandSelection(state, selection);
    if (materialId == UINT32_MAX) {
        return std::nullopt;
    }
    const AssetRecord* record = materialAssetRecordForLoadedMaterialCommand(state, materialId);
    if (record == nullptr) {
        return std::nullopt;
    }
    return state.dirtyMaterialAssets->find(record->guid) != state.dirtyMaterialAssets->end()
        ? std::optional<AssetGuid>{record->guid}
        : std::nullopt;
}

std::string commandUnavailableReason(EditorCommandId id, const EditorRuntimeState& state, const EditorSelection& selection) {
    switch (id) {
    case EditorCommandId::ProjectSettings:
        return state.project == nullptr ? "No project is currently open." : std::string{};
    case EditorCommandId::CloseProject:
        return state.project == nullptr ? "No project is currently open." : std::string{};
    case EditorCommandId::OpenProjectDirectory:
        return state.project == nullptr ? "No project is currently open." : std::string{};
    case EditorCommandId::SaveMaterial:
        if (state.project == nullptr) {
            return "No project is currently open.";
        }
        return selectedDirtyMaterialAssetGuid(state, selection).has_value()
            ? std::string{}
            : std::string("No dirty linked material asset is selected.");
    case EditorCommandId::Undo:
        return state.undoStack == nullptr || !state.undoStack->canUndo() ? "Nothing to undo." : std::string{};
    case EditorCommandId::Redo:
        return state.undoStack == nullptr || !state.undoStack->canRedo() ? "Nothing to redo." : std::string{};
    case EditorCommandId::ViewportFrameSelected:
        return !selection.entityId().valid() ? "No entity is selected." : std::string{};
    default:
        return {};
    }
}

std::filesystem::path canonicalForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return std::filesystem::absolute(path, ec);
}

std::string trimWhitespace(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

std::string normalizeConsoleCommandToken(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return ch == ' ' || ch == '.' || ch == '-' ? '_' : static_cast<char>(std::tolower(ch));
    });
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
#ifdef _WIN32
    constexpr const char* stderrRedirect = " 2>NUL";
#else
    constexpr const char* stderrRedirect = " 2>/dev/null";
#endif
    const std::string output = readCommandOutput("git -C " + quoteCommandPath(*gitRoot) + " status --porcelain -- " + quoteCommandPath(relative) + stderrRedirect);
    if (trimWhitespace(output).empty()) {
        return "Clean";
    }
    const std::string code = output.size() >= 2 ? output.substr(0, 2) : trimWhitespace(output);
    if (code == "??") return "Untracked";
    if (code.find('A') != std::string::npos) return "Added";
    if (code.find('M') != std::string::npos) return "Modified";
    if (code.find('D') != std::string::npos) return "Deleted";
    if (code.find('R') != std::string::npos) return "Renamed";
    if (code.find('C') != std::string::npos) return "Copied";
    if (code.find('U') != std::string::npos) return "Conflict";
    return "Changed";
}

ImVec4 sourceControlStatusColor(const std::string& status) {
    if (status == "Clean") return ImVec4(0.54f, 0.82f, 0.60f, 1.0f);
    if (status == "Modified" || status == "Added" || status == "Renamed" || status == "Copied") return ImVec4(0.95f, 0.68f, 0.28f, 1.0f);
    if (status == "Deleted" || status == "Conflict") return ImVec4(0.95f, 0.36f, 0.32f, 1.0f);
    if (status == "Untracked") return ImVec4(0.55f, 0.72f, 0.95f, 1.0f);
    return ImVec4(0.65f, 0.70f, 0.78f, 1.0f);
}

void drawProjectSaveStateRow(const char* label, bool dirty) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(150.0f);
    ImGui::TextColored(
        dirty ? ImVec4(0.95f, 0.70f, 0.25f, 1.0f) : ImVec4(0.55f, 0.75f, 0.58f, 1.0f),
        "%s",
        dirty ? "Dirty" : "Saved");
}

void drawProjectSourceControlRow(
    const char* label,
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& path,
    std::unordered_map<std::string, std::string>& cache) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(150.0f);
    if (path.empty()) {
        ImGui::TextDisabled("Unavailable");
        return;
    }
    const std::string key = canonicalForCompare(path).string();
    auto it = cache.find(key);
    if (it == cache.end()) {
        it = cache.emplace(key, gitStatusLabelForPath(workspaceRoot, path)).first;
    }
    ImGui::TextColored(sourceControlStatusColor(it->second), "%s", it->second.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", path.string().c_str());
    }
}

void drawProjectSaveState(const ProjectManagerRuntimeState& state, std::unordered_map<std::string, std::string>& sourceControlCache) {
    const bool registryAvailable = state.assetRegistry != nullptr && !state.assetRegistry->state().path.empty();
    const bool registryDirty = state.assetRegistry != nullptr && state.assetRegistry->dirty();
    ImGui::SeparatorText("Save State");
    drawProjectSaveStateRow("Current Level", state.sceneDirty);
    drawProjectSaveStateRow("Project Settings", state.projectSettingsDirty);
    drawProjectSaveStateRow("Asset Registry", registryDirty);
    if (registryAvailable) {
        ImGui::TextWrapped("Registry: %s", state.assetRegistry->state().path.string().c_str());
    } else {
        ImGui::TextDisabled("Registry: not loaded");
    }
    if (state.project != nullptr) {
        const std::filesystem::path workspaceRoot = state.project->projectRoot;
        const std::filesystem::path levelPath = state.scenePath != nullptr && state.scenePath->has_value()
            ? **state.scenePath
            : state.project->startupScene;
        ImGui::SeparatorText("Source Control");
        if (ImGui::SmallButton("Refresh Project Source Control")) {
            sourceControlCache.clear();
        }
        drawProjectSourceControlRow("Current Level", workspaceRoot, levelPath, sourceControlCache);
        drawProjectSourceControlRow("Project File", workspaceRoot, state.project->projectFile, sourceControlCache);
        drawProjectSourceControlRow("Asset Registry", workspaceRoot, registryAvailable ? state.assetRegistry->state().path : std::filesystem::path{}, sourceControlCache);
        ImGui::TextDisabled("Read-only Git status for project-owned level, settings, and registry files.");
    }
}

std::string projectCreationValidationMessage(
    const std::string& name,
    const std::filesystem::path& location,
    std::filesystem::path& previewPath) {
    if (name.empty()) {
        return "Project name is required.";
    }
    if (hasInvalidWindowsPathCharacter(name)) {
        return "Project name contains an invalid Windows path character.";
    }
    if (location.empty()) {
        return "Project location is required.";
    }
    previewPath = location / name / (name + ".vproject");
    const std::filesystem::path legacyPath = location / name / (name + ".rtproject");
    std::error_code ec;
    if (std::filesystem::exists(previewPath, ec) || std::filesystem::exists(legacyPath, ec)) {
        return "A project file already exists at this location.";
    }
    if (std::filesystem::exists(location, ec) && !std::filesystem::is_directory(location, ec)) {
        return "Project location is not a directory.";
    }
    const std::filesystem::path writableProbe = std::filesystem::exists(location, ec) ? location : nearestExistingParent(location);
    if (writableProbe.empty() || !pathLooksWritable(writableProbe)) {
        return "Project location is not writable.";
    }
    return {};
}

ImU32 projectCardAccent(const char* title) {
    uint32_t value = 0;
    for (const char* p = title; p != nullptr && *p != '\0'; ++p) {
        value = value * 33u + static_cast<unsigned char>(*p);
    }
    const int r = 45 + static_cast<int>(value % 55u);
    const int g = 70 + static_cast<int>((value / 7u) % 70u);
    const int b = 100 + static_cast<int>((value / 17u) % 95u);
    return IM_COL32(r, g, b, 255);
}

std::string lowercase(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool statusContainsAny(std::string status, std::initializer_list<const char*> needles) {
    status = lowercase(std::move(status));
    for (const char* needle : needles) {
        if (status.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool statusLooksFailed(const std::string& status) {
    return statusContainsAny(status, {"failed", "failure", "error"});
}

bool statusLooksCancelled(const std::string& status) {
    return statusContainsAny(status, {"cancelled", "canceled", "stopped"});
}

struct ProjectCardThumbnail {
    bool attempted = false;
    bool available = false;
    int width = 0;
    int height = 0;
    int columns = 12;
    int rows = 5;
    std::vector<uint32_t> colors;
};

bool isProjectThumbnailPath(const std::filesystem::path& path) {
    const std::string ext = lowercase(path.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
}

ProjectCardThumbnail& projectCardThumbnail(const std::filesystem::path& path) {
    static std::unordered_map<std::string, ProjectCardThumbnail> cache;
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::string key = ec ? path.string() : absolute.string();
    ProjectCardThumbnail& thumbnail = cache[key];
    if (thumbnail.attempted) {
        return thumbnail;
    }
    thumbnail.attempted = true;
    thumbnail.colors.assign(static_cast<size_t>(thumbnail.columns * thumbnail.rows), IM_COL32(32, 38, 46, 255));
    if (!isProjectThumbnailPath(path) || !std::filesystem::exists(path, ec)) {
        return thumbnail;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
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
            thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)] = IM_COL32(data[index], data[index + 1], data[index + 2], 255);
        }
    }
    stbi_image_free(data);
    return thumbnail;
}

bool drawProjectCardThumbnail(const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    ProjectCardThumbnail& thumbnail = projectCardThumbnail(path);
    if (!thumbnail.available) {
        return false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), 3.0f);
    const float cellW = (max.x - min.x) / static_cast<float>(thumbnail.columns);
    const float cellH = (max.y - min.y) / static_cast<float>(thumbnail.rows);
    for (int row = 0; row < thumbnail.rows; ++row) {
        for (int col = 0; col < thumbnail.columns; ++col) {
            const ImVec2 cellMin(min.x + static_cast<float>(col) * cellW, min.y + static_cast<float>(row) * cellH);
            const ImVec2 cellMax(min.x + static_cast<float>(col + 1) * cellW + 0.5f, min.y + static_cast<float>(row + 1) * cellH + 0.5f);
            dl->AddRectFilled(cellMin, cellMax, thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)]);
        }
    }
    dl->AddRect(min, max, IM_COL32(255, 255, 255, 38), 3.0f);
    return true;
}

bool projectManagerCard(
    const char* title,
    const char* detail,
    bool selected,
    const ImVec2& size,
    EditorGlyphIcon icon = EditorGlyphIcon::ProjectFile,
    const char* badge = nullptr,
    const std::filesystem::path* thumbnailPath = nullptr) {
    const bool clicked = ImGui::InvisibleButton(title, size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float rounding = EditorUiMetric::cardRounding;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = selected ? IM_COL32(24, 43, 72, 255) : hovered ? IM_COL32(28, 34, 44, 255) : IM_COL32(18, 21, 27, 255);
    const ImU32 border = selected ? IM_COL32(70, 135, 230, 255) : hovered ? IM_COL32(70, 78, 92, 255) : IM_COL32(42, 47, 56, 255);
    dl->AddRectFilled(min, max, bg, rounding);
    dl->AddRect(min, max, border, rounding, 0, selected ? 2.0f : 1.0f);

    const float previewHeight = size.y >= 112.0f ? EditorUiMetric::projectCardPreviewHeight : 32.0f;
    const ImVec2 previewMin(min.x + EditorUiMetric::cardPadding, min.y + EditorUiMetric::cardPadding);
    const ImVec2 previewMax(max.x - EditorUiMetric::cardPadding, min.y + previewHeight);
    const bool thumbnailDrawn = thumbnailPath != nullptr && drawProjectCardThumbnail(*thumbnailPath, previewMin, previewMax);
    if (!thumbnailDrawn) {
        dl->AddRectFilled(previewMin, previewMax, projectCardAccent(title), 3.0f);
    }
    dl->AddRectFilled(
        ImVec2(previewMin.x, previewMax.y - 18.0f),
        previewMax,
        IM_COL32(8, 11, 16, 86),
        3.0f,
        ImDrawFlags_RoundCornersBottom);
    if (!thumbnailDrawn) {
        const ImVec2 glyphSize(34.0f, 34.0f);
        editorDrawIconGlyph(
            icon,
            ImVec2(previewMin.x + (previewMax.x - previewMin.x - glyphSize.x) * 0.5f, previewMin.y + (previewMax.y - previewMin.y - glyphSize.y) * 0.5f),
            ImVec2(previewMin.x + (previewMax.x - previewMin.x + glyphSize.x) * 0.5f, previewMin.y + (previewMax.y - previewMin.y + glyphSize.y) * 0.5f),
            IM_COL32(218, 226, 238, 255));
    }
    const float textY = min.y + previewHeight + 12.0f;
    dl->AddText(ImVec2(min.x + 10.0f, textY), IM_COL32(224, 228, 234, 255), title);
    dl->AddText(ImVec2(min.x + 10.0f, textY + 20.0f), IM_COL32(150, 156, 166, 255), detail);
    if (badge != nullptr && badge[0] != '\0') {
        const ImVec2 badgeSize = ImGui::CalcTextSize(badge);
        const ImVec2 badgeMin(max.x - badgeSize.x - 18.0f, max.y - 24.0f);
        dl->AddRectFilled(badgeMin, ImVec2(max.x - 8.0f, max.y - 7.0f), IM_COL32(35, 42, 54, 230), 3.0f);
        dl->AddText(ImVec2(badgeMin.x + 5.0f, badgeMin.y + 2.0f), IM_COL32(170, 180, 194, 255), badge);
    }
    return clicked;
}

EditorGlyphIcon projectTemplateGlyph(int templateIndex) {
    switch (templateIndex) {
    case 1:
        return EditorGlyphIcon::Light;
    case 2:
        return EditorGlyphIcon::Environment;
    case 3:
        return EditorGlyphIcon::SceneFile;
    case 4:
        return EditorGlyphIcon::Stats;
    case 5:
        return EditorGlyphIcon::Camera;
    default:
        return EditorGlyphIcon::ProjectFile;
    }
}

void queueSampleProjectOpen(const std::filesystem::path& path, bool importAsScene, EditorRequests& requests) {
    if (importAsScene) {
        requests.importSceneAsNewScene = path;
    } else {
        requests.openScene = path;
    }
}

} // namespace

EditorRequests EditorLayer::draw(EditorRuntimeState& state) {
    EditorRequests requests;
    state.editorPrefs = &editorPrefs_;
    state.cameraBookmarks = &cameraBookmarks_;
    state.log = &log_;
    state.timeline = &timeline_;
    ImGui::GetIO().FontGlobalScale = std::clamp(editorPrefs_.uiScale, 0.75f, 1.75f);
    ImGuiIO& io = ImGui::GetIO();
    const EditorKeybinding commandPaletteBinding = editorCommandKeybinding(EditorCommandId::CommandPalette, &editorPrefs_);
    if (!io.WantTextInput && commandPaletteBinding.imguiKey >= 0 &&
        commandPaletteBinding.ctrl == io.KeyCtrl &&
        commandPaletteBinding.shift == io.KeyShift &&
        commandPaletteBinding.alt == io.KeyAlt &&
        ImGui::IsKeyPressed(static_cast<ImGuiKey>(commandPaletteBinding.imguiKey))) {
        commandPaletteOpen_ = true;
    }
    applyThemePreset();
    applyWorkspacePreset();
    const bool timelineAdvanced = timeline_.advance(state.cpuFrameMs / 1000.0f);
    if (timelineAdvanced && state.sceneDocument != nullptr) {
        for (uint64_t uuid : timeline_.animatedEntityUuids()) {
            Transform sampled;
            if (!timeline_.sampleTransform(uuid, timeline_.currentFrame, sampled)) {
                continue;
            }
            for (const Entity* entity : state.sceneDocument->registry().entities()) {
                if (entity != nullptr && entity->uuid == uuid) {
                    requests.timelinePlaybackTransforms.push_back(EditorTimelineTransformSample{entity->id, sampled});
                    break;
                }
            }
        }
    }
    if (state.project != nullptr) {
        dockspace_.setProfileFile(state.project->savedRoot / "Editor" / "layout.ini");
    } else if (state.sceneDocument != nullptr && state.sceneDocument->sourceGltfPath().has_value()) {
        dockspace_.setProfilePath(*state.sceneDocument->sourceGltfPath());
    } else if (state.gltfPath != nullptr && state.gltfPath->has_value()) {
        dockspace_.setProfilePath(**state.gltfPath);
    }
    if (state.placement != nullptr && state.placement->serial != 0 && state.placement->serial != observedPlacementSerial_) {
        observedPlacementSerial_ = state.placement->serial;
        if (state.placement->entity.valid() && state.sceneDocument != nullptr && state.sceneDocument->registry().entity(state.placement->entity) != nullptr) {
            selection_.selectEntity(state.placement->entity);
            selection_.setLastClickedId(state.placement->entity);
            log_.add(EditorLogCategory::Scene, state.placement->label.empty() ? "Placed asset selected" : state.placement->label + " selected");
        }
    }
    updateJobCenterHistory(state);
    dockspace_.begin(state, visibility_, requests);

    if (requests.showProjectManager) {
        showProjectManager_ = true;
        projectManagerDismissed_ = false;
    }
    if (requests.showProjectSettings) {
        showProjectManager_ = true;
        projectManagerDismissed_ = false;
        projectManagerSection_ = 2;
    }
    if (requests.showCommandPalette) {
        commandPaletteOpen_ = true;
    }
    const bool projectManagerGateActive = showProjectManager_ || (state.project == nullptr && !projectManagerDismissed_);
    if (projectManagerGateActive) {
        drawProjectManager(ProjectManagerRuntimeState{
            .project = state.project,
            .assetRegistry = state.assetRegistry,
            .scenePath = state.scenePath,
            .sceneLoadingStatus = state.sceneLoadingStatus,
            .sceneLoadRunning = state.sceneLoadRunning,
            .sceneLoadProgress = state.sceneLoadProgress,
            .sceneDirty = state.sceneDirty,
            .projectSettingsDirty = state.projectSettingsDirty,
        }, requests);
    }
    if (recoveryPromptVisible_) {
        drawRecoveryPrompt(requests);
    }
    if (state.sceneLoadRunning) {
        drawSceneLoadingOverlay(state, requests);
    }
    drawRenderJobModal(state, requests);

    if (projectManagerGateActive && state.project == nullptr) {
        drawCommandPalette(state, requests);
        dockspace_.end(visibility_, requests);
        return requests;
    }

    if (visibility_.viewport) {
        viewportPanel_.draw(state, selection_, requests);
    }
    if (visibility_.sceneHierarchy) {
        sceneHierarchyPanel_.draw(state, selection_, requests);
    }
    if (visibility_.renderWorldSettings) {
        drawRenderWorldSettingsPanel(state, requests);
    }
    if (visibility_.inspector) {
        inspectorPanel_.draw(state, selection_, requests);
    }
    if (requests.openSelectedAsset) {
        visibility_.assetBrowser = true;
        assetBrowserPanel_.openSelectedAsset(state, selection_, requests);
        requests.openSelectedAsset = false;
    }
    if (requests.showMaterialEditor) {
        visibility_.materialEditor = true;
    }
    if (visibility_.assetBrowser) {
        assetBrowserPanel_.draw(state, selection_, requests);
    }
    if (visibility_.materialEditor) {
        materialEditorPanel_.draw(state, selection_, requests);
        if (requests.closeMaterialEditor) {
            visibility_.materialEditor = false;
        }
    }
    if (visibility_.renderSettings) {
        renderSettingsPanel_.draw(state, requests);
    }
    if (visibility_.debugProfiler) {
        debugProfilerPanel_.draw(state, requests);
    }
    if (visibility_.sceneStats) {
        sceneStatsPanel_.draw(state);
    }
    if (visibility_.gpuDiagnostics) {
        gpuDiagnosticsPanel_.draw(state);
    }
    if (visibility_.jobCenter) {
        drawJobCenterPanel(state, requests);
    }
    if (visibility_.timeline) {
        drawTimelinePanel(state, requests);
    }
    if (visibility_.log) {
        drawLogPanel(state, requests);
    }
    if (visibility_.console) {
        drawConsolePanel(state, requests);
    }
    applyCaptureFocusOverride();
    drawCommandPalette(state, requests);

    dockspace_.end(visibility_, requests);
    return requests;
}

void EditorLayer::applyCaptureFocusOverride() {
    if (!captureFocusOverrideInitialized_) {
        captureFocusOverrideInitialized_ = true;
#if defined(_WIN32)
        char* value = nullptr;
        size_t valueLength = 0;
        if (_dupenv_s(&value, &valueLength, "RTV_EDITOR_CAPTURE_FOCUS_WINDOW") == 0 && value != nullptr) {
            captureFocusWindow_ = value;
            std::free(value);
        }
#else
        if (const char* value = std::getenv("RTV_EDITOR_CAPTURE_FOCUS_WINDOW")) {
            captureFocusWindow_ = value;
        }
#endif
        const bool supportedWindow =
            captureFocusWindow_ == "Scene" ||
            captureFocusWindow_ == "Hierarchy" ||
            captureFocusWindow_ == "Render Settings" ||
            captureFocusWindow_ == "Render World Settings" ||
            captureFocusWindow_ == "Inspector" ||
            captureFocusWindow_ == "Material Editor" ||
            captureFocusWindow_ == "Content" ||
            captureFocusWindow_ == "Timeline" ||
            captureFocusWindow_ == "Log" ||
            captureFocusWindow_ == "Job Center";
        if (!supportedWindow) {
            captureFocusWindow_.clear();
        }
        if (captureFocusWindow_ == "Render World Settings") {
            captureFocusWindow_ = "Render Settings";
        }
        captureFocusFramesRemaining_ = captureFocusWindow_.empty() ? 0 : 45;
    }

    if (captureFocusFramesRemaining_ > 0 && !captureFocusWindow_.empty()) {
        ImGui::SetWindowFocus(captureFocusWindow_.c_str());
        --captureFocusFramesRemaining_;
    }
}

EditorRequests EditorLayer::drawProjectManagerLauncher(ProjectManagerRuntimeState state) {
    EditorRequests requests;
    ImGui::GetIO().FontGlobalScale = std::clamp(editorPrefs_.uiScale, 0.75f, 1.75f);
    applyThemePreset();
    drawProjectManager(state, requests);
    return requests;
}

void EditorLayer::resetLayout() {
    dockspace_.requestResetLayout();
}

void EditorLayer::handleNotificationAction(NotificationAction action, EditorRequests& requests) {
    switch (action) {
    case NotificationAction::OpenLog:
        visibility_.log = true;
        break;
    case NotificationAction::OpenContent:
        visibility_.assetBrowser = true;
        break;
    case NotificationAction::OpenRenderSettings:
        visibility_.renderSettings = true;
        break;
    case NotificationAction::OpenProjectManager:
        requests.showProjectManager = true;
        showProjectManager_ = true;
        projectManagerDismissed_ = false;
        break;
    case NotificationAction::OpenOutputFolder:
        requests.openOutputFolder = true;
        break;
    case NotificationAction::None:
    default:
        break;
    }
}

void EditorLayer::showRecoveryPrompt(
    std::filesystem::path markerPath,
    std::filesystem::path autosavePath,
    std::filesystem::path projectAutosavePath,
    std::filesystem::path assetRegistryAutosavePath,
    std::vector<std::pair<std::string, std::filesystem::path>> materialAssetAutosavePaths) {
    recoveryPromptVisible_ = true;
    recoveryMarkerPath_ = std::move(markerPath);
    recoveryAutosavePath_ = std::move(autosavePath);
    recoveryProjectAutosavePath_ = std::move(projectAutosavePath);
    recoveryAssetRegistryAutosavePath_ = std::move(assetRegistryAutosavePath);
    recoveryMaterialAssetAutosavePaths_ = std::move(materialAssetAutosavePaths);
}

void EditorLayer::drawRecoveryPrompt(EditorRequests& requests) {
    ImGui::OpenPopup("Recovery Available");
    if (ImGui::BeginPopupModal("Recovery Available", &recoveryPromptVisible_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("A previous editor session marker was found. You can restore the latest autosave if it exists, or discard the recovery marker.");
        ImGui::Separator();
        ImGui::TextWrapped("Marker: %s", recoveryMarkerPath_.string().c_str());
        ImGui::TextWrapped("Level autosave: %s", recoveryAutosavePath_.string().c_str());
        if (!recoveryProjectAutosavePath_.empty()) {
            ImGui::TextWrapped("Project autosave: %s", recoveryProjectAutosavePath_.string().c_str());
        }
        if (!recoveryAssetRegistryAutosavePath_.empty()) {
            ImGui::TextWrapped("Asset registry autosave: %s", recoveryAssetRegistryAutosavePath_.string().c_str());
        }
        if (!recoveryMaterialAssetAutosavePaths_.empty()) {
            ImGui::TextWrapped("Material asset autosaves: %zu", recoveryMaterialAssetAutosavePaths_.size());
            const size_t previewCount = std::min<size_t>(recoveryMaterialAssetAutosavePaths_.size(), 3u);
            for (size_t i = 0; i < previewCount; ++i) {
                const auto& [guid, path] = recoveryMaterialAssetAutosavePaths_[i];
                ImGui::TextWrapped("  %s: %s", guid.empty() ? "material" : guid.c_str(), path.string().c_str());
            }
            if (recoveryMaterialAssetAutosavePaths_.size() > previewCount) {
                ImGui::TextDisabled("Additional material autosaves: %zu", recoveryMaterialAssetAutosavePaths_.size() - previewCount);
            }
        }
        const bool canRestoreLevel = std::filesystem::exists(recoveryAutosavePath_);
        const bool canRestoreProject = !recoveryProjectAutosavePath_.empty() && std::filesystem::exists(recoveryProjectAutosavePath_);
        const bool canRestoreAssetRegistry = !recoveryAssetRegistryAutosavePath_.empty() && std::filesystem::exists(recoveryAssetRegistryAutosavePath_);
        const bool canRestoreMaterial = std::any_of(
            recoveryMaterialAssetAutosavePaths_.begin(),
            recoveryMaterialAssetAutosavePaths_.end(),
            [](const auto& item) { return !item.second.empty() && std::filesystem::exists(item.second); });
        const bool canRestore = canRestoreLevel || canRestoreProject || canRestoreAssetRegistry || canRestoreMaterial;
        if (!canRestore) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Restore Autosaves", ImVec2(160.0f, 0.0f))) {
            requests.restoreAutosave = true;
            recoveryPromptVisible_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (!canRestore) {
            ImGui::EndDisabled();
        }
        if (!canRestore) {
            ImGui::SameLine();
            ImGui::TextDisabled("No autosaves found");
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard Recovery", ImVec2(160.0f, 0.0f))) {
            requests.discardRecovery = true;
            recoveryPromptVisible_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later", ImVec2(100.0f, 0.0f))) {
            recoveryPromptVisible_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::drawSceneLoadingOverlay(const EditorRuntimeState& state, EditorRequests& requests) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 72.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("Scene Loading Overlay", nullptr, flags)) {
        ImGui::TextUnformatted("Scene Loading");
        ImGui::ProgressBar(std::clamp(state.sceneLoadProgress, 0.0f, 1.0f), ImVec2(360.0f, 0.0f));
        if (state.sceneLoadingStatus != nullptr && !state.sceneLoadingStatus->empty()) {
            ImGui::TextWrapped("%s", state.sceneLoadingStatus->c_str());
        }
        if (ImGui::Button("Cancel")) {
            requests.cancelSceneLoad = true;
        }
    }
    ImGui::End();
}

void EditorLayer::drawRenderJobModal(const EditorRuntimeState& state, EditorRequests& requests) {
    const EditorRenderJobStatus* job = state.renderJob;
    if (job == nullptr || job->kind == EditorRenderJobKind::None) {
        return;
    }

    if (job->serial != observedRenderJobSerial_) {
        observedRenderJobSerial_ = job->serial;
        renderJobModalOpen_ = true;
        ImGui::OpenPopup("Render Output");
    }
    if (!renderJobModalOpen_) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Render Output", &renderJobModalOpen_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(job->title.empty() ? "Render" : job->title.c_str());
        ImGui::SameLine();
        if (job->active) {
            ImGui::TextColored(ImVec4(0.40f, 0.68f, 1.0f, 1.0f), "Active");
        } else if (job->completed) {
            ImGui::TextColored(ImVec4(0.42f, 0.82f, 0.52f, 1.0f), "Complete");
        } else if (job->cancelled) {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f), "Stopped");
        } else if (job->failed) {
            ImGui::TextColored(ImVec4(0.95f, 0.34f, 0.34f, 1.0f), "Failed");
        }

        ImGui::Separator();
        ImGui::ProgressBar(std::clamp(job->progress, 0.0f, 1.0f), ImVec2(480.0f, 0.0f));
        if (!job->status.empty()) {
            ImGui::TextWrapped("%s", job->status.c_str());
        }
        if (job->totalFrames > 1) {
            ImGui::TextDisabled("Frames: %d / %d", std::clamp(job->currentFrame, 0, job->totalFrames), job->totalFrames);
        }
        if (!job->outputRoot.empty()) {
            ImGui::TextDisabled("Output: %s", job->outputRoot.string().c_str());
        }
        if (!job->manifestPath.empty()) {
            ImGui::TextDisabled("Manifest: %s", job->manifestPath.filename().string().c_str());
        }

        ImGui::Separator();
        if (editorIconTextButton("RenderJobOpenOutput", EditorGlyphIcon::Folder, "Open Output")) {
            requests.openOutputFolder = true;
        }
        ImGui::SameLine();
        if (job->active) {
            if (editorIconTextButton("RenderJobStop", EditorGlyphIcon::Stop, "Stop Render")) {
                requests.stopRender = true;
            }
        } else if (editorIconTextButton("RenderJobClose", EditorGlyphIcon::Exit, "Close")) {
            renderJobModalOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

VkExtent2D EditorLayer::desiredRenderExtent(VkExtent2D fallback) const {
    return viewportPanel_.desiredRenderExtent(fallback);
}

bool EditorLayer::viewportInteractionActive() const {
    return viewportPanel_.interactionActive();
}

bool EditorLayer::viewportHovered() const {
    return viewportPanel_.hovered();
}

void EditorLayer::drawProjectManager(const ProjectManagerRuntimeState& state, EditorRequests& requests) {
    if (newProjectName_[0] == '\0') {
        const char* defaultName = "MyProject";
        std::memcpy(newProjectName_.data(), defaultName, std::strlen(defaultName));
    }
    if (newProjectLocation_[0] == '\0') {
        const std::string defaultLocation = defaultVibodeProjectRoot().string();
        std::memcpy(newProjectLocation_.data(), defaultLocation.data(), std::min(defaultLocation.size(), newProjectLocation_.size() - 1));
    }

    ImGuiWindowClass nativeWindowClass{};
    nativeWindowClass.ClassId = 0x50524d47u;
    nativeWindowClass.DockingAllowUnclassed = false;
    nativeWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&nativeWindowClass);
    if (ImGuiViewport* mainViewport = ImGui::GetMainViewport()) {
        ImGui::SetNextWindowPos(ImVec2(mainViewport->WorkPos.x + 64.0f, mainViewport->WorkPos.y + 48.0f), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(920.0f, 620.0f), ImGuiCond_FirstUseEver);
    bool open = showProjectManager_ || state.project == nullptr;
    constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking;
    if (!ImGui::Begin("Vibode Engine Project Manager###Project Manager", &open, windowFlags)) {
        ImGui::End();
        showProjectManager_ = open;
        return;
    }

    const char* templates[] = {"Empty", "Basic Lit", "Outdoor / Atmosphere", "Interior", "Path Tracing Validation", "Cinematic"};
    const char* templateDescriptions[] = {
        "Folders only",
        "Camera, sun, environment",
        "Sky, fog, atmosphere",
        "Area lighting setup",
        "Deterministic validation scene",
        "Camera and post process"
    };
    newProjectTemplate_ = std::clamp(newProjectTemplate_, 0, 5);

    ImGui::TextUnformatted("Vibode Engine");
    ImGui::SameLine();
    ImGui::TextDisabled(state.standaloneLauncher ? "Project Manager Launcher" : "Project Manager");
    if (state.project == nullptr) {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.25f, 1.0f), "No project is currently open.");
    } else {
        ImGui::Text("Current project: %s", state.project->name.c_str());
    }
    if (state.sceneLoadRunning && state.sceneLoadingStatus != nullptr) {
        ImGui::ProgressBar(std::clamp(state.sceneLoadProgress, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), state.sceneLoadingStatus->c_str());
    } else if (state.standaloneLauncher) {
        ImGui::TextDisabled("Renderer startup is deferred until a project or scene is selected.");
    }
    if (state.project != nullptr || state.sceneDirty || state.assetRegistry != nullptr) {
        drawProjectSaveState(state, projectSourceControlStatusCache_);
    }

    int startupMode = editorPrefs_.openLastProject ? 1 : 0;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("Load on Startup", &startupMode, "Show Project Manager\0Open Last Project\0")) {
        editorPrefs_.openLastProject = startupMode == 1;
        editorPrefs_.save(EditorPreferences::defaultPath());
    }
    if (state.project == nullptr) {
        ImGui::Spacing();
        if (ImGui::Button("Create Project##ProjectManagerPrimary", ImVec2(160.0f, 0.0f))) {
            projectManagerSection_ = 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Project##ProjectManagerPrimary", ImVec2(150.0f, 0.0f))) {
            projectManagerSection_ = 4;
        }
    }

    ImGui::Separator();
    ImGui::BeginChild("ProjectManagerRail", ImVec2(EditorUiMetric::sidebarWidth, 0.0f), true);
    const char* sections[] = {"Home", "Create Project", "My Projects", "Sample Projects", "Open Project"};
    for (int i = 0; i < 5; ++i) {
        if (ImGui::Selectable(sections[i], projectManagerSection_ == i, 0, ImVec2(0.0f, 30.0f))) {
            projectManagerSection_ = i;
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("Resources");
    ImGui::TextDisabled("Documentation");
    ImGui::TextDisabled("Community");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("ProjectManagerContent", ImVec2(0.0f, 0.0f), true);

    if (projectManagerSection_ == 0) {
        ImGui::SeparatorText("Recent Projects");
        if (editorPrefs_.recentProjects.empty()) {
            ImGui::TextDisabled("No recent projects yet.");
        }
        const float cardWidth = EditorUiMetric::projectCardWidth;
        const float cardHeight = EditorUiMetric::projectCardHeight;
        int column = 0;
        for (size_t i = 0; i < editorPrefs_.recentProjects.size(); ++i) {
            const std::string& project = editorPrefs_.recentProjects[i];
            const std::filesystem::path projectPath(project);
            const bool missing = !std::filesystem::exists(projectPath);
            const std::filesystem::path thumbnailA = projectPath.parent_path() / "Saved" / "Thumbnail.png";
            const std::filesystem::path thumbnailB = projectPath.parent_path() / "Saved" / "ProjectThumbnail.png";
            const bool hasThumbnail = std::filesystem::exists(thumbnailA) || std::filesystem::exists(thumbnailB);
            const std::filesystem::path thumbnailPath = std::filesystem::exists(thumbnailA) ? thumbnailA : thumbnailB;
            const bool selected = state.project != nullptr && canonicalForCompare(state.project->projectFile) == canonicalForCompare(projectPath);
            ImGui::PushID(static_cast<int>(i));
            const std::string detail = missing ? "Project file missing" : projectPath.parent_path().filename().string();
            const char* badge = missing ? "Missing" : hasThumbnail ? "Thumbnail" : selected ? "Current" : "Recent";
            if (projectManagerCard(projectPath.stem().string().c_str(), detail.c_str(), selected, ImVec2(cardWidth, cardHeight), EditorGlyphIcon::ProjectFile, badge, hasThumbnail ? &thumbnailPath : nullptr) && !missing) {
                requests.openProject = OpenProjectRequest{projectPath};
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Open", nullptr, false, !missing)) {
                    requests.openProject = OpenProjectRequest{projectPath};
                }
                if (ImGui::MenuItem("Remove from Recent")) {
                    editorPrefs_.removeRecentProject(project);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            if (++column % 3 != 0) {
                ImGui::SameLine();
            }
        }
        ImGui::SeparatorText("Quick Start");
        if (ImGui::Button("Create Project", ImVec2(150.0f, 0.0f))) {
            projectManagerSection_ = 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Project", ImVec2(140.0f, 0.0f))) {
            projectManagerSection_ = 4;
        }
        if (!editorPrefs_.lastOpenedProject.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Open Last", ImVec2(140.0f, 0.0f))) {
                requests.openProject = OpenProjectRequest{editorPrefs_.lastOpenedProject};
            }
        }
    } else if (projectManagerSection_ == 1) {
        ImGui::SeparatorText("Templates");
        for (int i = 0; i < 6; ++i) {
            ImGui::PushID(i);
            if (projectManagerCard(templates[i], templateDescriptions[i], newProjectTemplate_ == i, ImVec2(EditorUiMetric::projectTemplateCardWidth, 120.0f), projectTemplateGlyph(i), newProjectTemplate_ == i ? "Selected" : "Template")) {
                newProjectTemplate_ = i;
            }
            ImGui::PopID();
            if ((i + 1) % 3 != 0) {
                ImGui::SameLine();
            }
        }
        ImGui::SeparatorText("Project");
        ImGui::TextUnformatted("Project Name");
        ImGui::SetNextItemWidth(std::min(540.0f, ImGui::GetContentRegionAvail().x));
        ImGui::InputText("##ProjectName", newProjectName_.data(), newProjectName_.size());
        ImGui::TextUnformatted("Location");
        const float browseWidth = 92.0f;
        ImGui::SetNextItemWidth(std::max(220.0f, ImGui::GetContentRegionAvail().x - browseWidth - ImGui::GetStyle().ItemSpacing.x));
        ImGui::InputText("##ProjectLocation", newProjectLocation_.data(), newProjectLocation_.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse##ProjectLocation")) {
            if (auto folder = openFolderDialog(L"Select Vibode Project Location")) {
                copyTextToBuffer(newProjectLocation_, folder->string());
            }
        }
        std::filesystem::path preview;
        const std::string validation = projectCreationValidationMessage(newProjectName_.data(), std::filesystem::path(newProjectLocation_.data()), preview);
        ImGui::TextWrapped("Project file: %s", preview.empty() ? "(enter a valid name and location)" : preview.string().c_str());
        std::error_code parentEc;
        const std::filesystem::path locationPath(newProjectLocation_.data());
        if (!locationPath.empty() && !std::filesystem::exists(locationPath, parentEc) && validation.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 0.95f, 1.0f), "Parent folder will be created.");
        }
        if (!validation.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "%s", validation.c_str());
        }
        ImGui::Checkbox("Create Default Scene", &createDefaultScene_);
        ImGui::SameLine();
        ImGui::Checkbox("Create Default Content Folders", &createDefaultContentFolders_);
        if (!validation.empty()) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Create Project", ImVec2(160.0f, 0.0f))) {
            CreateProjectRequest request;
            request.name = newProjectName_.data();
            request.location = std::filesystem::path(newProjectLocation_.data());
            request.templateName = templates[newProjectTemplate_];
            request.createDefaultScene = createDefaultScene_;
            request.createDefaultContentFolders = createDefaultContentFolders_;
            requests.createProject = request;
        }
        if (!validation.empty()) {
            ImGui::EndDisabled();
        }
    } else if (projectManagerSection_ == 2) {
        ImGui::SeparatorText("My Projects");
        if (state.project != nullptr) {
            ImGui::Text("Current: %s", state.project->name.c_str());
            ImGui::TextWrapped("Root: %s", state.project->projectRoot.string().c_str());
            if (ImGui::Button("Open Current Project Directory")) {
                requests.openProjectDirectory = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Close Project")) {
                requests.closeProject = true;
            }
        } else {
            ImGui::TextDisabled("No project is currently open.");
        }
        ImGui::SeparatorText("Recent Projects");
        for (size_t i = 0; i < editorPrefs_.recentProjects.size(); ++i) {
            const std::string& project = editorPrefs_.recentProjects[i];
            const bool missing = !std::filesystem::exists(project);
            ImGui::PushID(static_cast<int>(i + 1000));
            if (ImGui::Selectable(std::filesystem::path(project).filename().string().c_str(), false, 0, ImVec2(0.0f, 28.0f)) && !missing) {
                requests.openProject = OpenProjectRequest{project};
            }
            if (missing) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "missing");
            }
            ImGui::PopID();
        }
    } else if (projectManagerSection_ == 3) {
        ImGui::SeparatorText("Sample Projects");
        struct SampleProjectCard {
            const char* title;
            const char* detail;
            std::filesystem::path projectFile;
            std::filesystem::path scenePath;
            bool importAsScene = false;
        };
        const SampleProjectCard samples[] = {
            {"Cornell Validation", "Fast deterministic smoke", std::filesystem::path("Samples/CornellValidation/CornellValidation.vproject"), std::filesystem::path("scenes/validation/cornell.rtlevel"), false},
            {"Lightweight Sponza", "Scene-loading coverage", std::filesystem::path("Samples/LightweightSponza/LightweightSponza.vproject"), std::filesystem::path("Sponza/glTF/Sponza.gltf"), true},
            {"Cinematic Lighting", "Close camera lighting check", std::filesystem::path("Samples/CinematicLighting/CinematicLighting.vproject"), std::filesystem::path("scenes/validation/closeup_cornell.rtlevel"), false},
        };
        for (int i = 0; i < 3; ++i) {
            const SampleProjectCard& sample = samples[i];
            const bool projectAvailable = std::filesystem::exists(sample.projectFile);
            const bool sceneAvailable = std::filesystem::exists(sample.scenePath);
            ImGui::PushID(i);
            ImGui::BeginGroup();
            if (projectManagerCard(
                    sample.title,
                    sample.detail,
                    false,
                    ImVec2(EditorUiMetric::projectCardWidth, EditorUiMetric::projectCardHeight),
                    sample.importAsScene ? EditorGlyphIcon::Model : EditorGlyphIcon::SceneFile,
                    projectAvailable ? "Project" : sceneAvailable ? "Scene" : "Missing")) {
                if (projectAvailable) {
                    requests.openProject = OpenProjectRequest{sample.projectFile};
                } else if (sceneAvailable) {
                    queueSampleProjectOpen(sample.scenePath, sample.importAsScene, requests);
                }
            }
            if (!projectAvailable && !sceneAvailable) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton(projectAvailable ? "Open Project" : sample.importAsScene ? "Import Scene" : "Open Scene")) {
                if (projectAvailable) {
                    requests.openProject = OpenProjectRequest{sample.projectFile};
                } else {
                    queueSampleProjectOpen(sample.scenePath, sample.importAsScene, requests);
                }
            }
            if (!projectAvailable && !sceneAvailable) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("Not found");
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", projectAvailable ? sample.projectFile.filename().string().c_str() : sample.scenePath.filename().string().c_str());
            }
            if (projectAvailable && sceneAvailable) {
                if (sample.importAsScene) {
                    if (ImGui::SmallButton("Import Asset Scene")) {
                        queueSampleProjectOpen(sample.scenePath, true, requests);
                    }
                } else {
                    if (ImGui::SmallButton("Open Scene Only")) {
                        queueSampleProjectOpen(sample.scenePath, false, requests);
                    }
                }
            }
            ImGui::EndGroup();
            ImGui::PopID();
            if (i != 2) {
                ImGui::SameLine();
            }
        }
    } else {
        ImGui::SeparatorText("Open Project");
        ImGui::InputText("Project File", openProjectPath_.data(), openProjectPath_.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse")) {
            if (auto path = openProjectFileDialog()) {
                copyTextToBuffer(openProjectPath_, path->string());
            }
        }
        const std::filesystem::path path(openProjectPath_.data());
        const bool canOpenProject = !path.empty() && std::filesystem::exists(path);
        if (!canOpenProject) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Open Project", ImVec2(150.0f, 0.0f))) {
            requests.openProject = OpenProjectRequest{path};
        }
        if (!canOpenProject) {
            ImGui::EndDisabled();
        }
        if (!editorPrefs_.lastOpenedProject.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Open Last Project", ImVec2(150.0f, 0.0f))) {
                requests.openProject = OpenProjectRequest{editorPrefs_.lastOpenedProject};
            }
        }
    }

    ImGui::EndChild();

    if (state.project == nullptr && ImGui::Button("Continue Without Project")) {
        showProjectManager_ = false;
        projectManagerDismissed_ = true;
        requests.continueWithoutProject = true;
    }
    if (state.project != nullptr) {
        ImGui::SameLine();
        if (ImGui::Button("Close Project")) {
            requests.closeProject = true;
        }
        ImGui::SeparatorText("Project Settings");
        bool autosaveEnabled = state.project->autosaveEnabled;
        int autosaveIntervalMinutes = state.project->autosaveIntervalMinutes;
        bool settingsChanged = false;
        settingsChanged |= ImGui::Checkbox("Autosave Enabled", &autosaveEnabled);
        settingsChanged |= ImGui::DragInt("Autosave Interval Minutes", &autosaveIntervalMinutes, 1.0f, 1, 120);
        if (settingsChanged) {
            ProjectContext next = *state.project;
            next.autosaveEnabled = autosaveEnabled;
            next.autosaveIntervalMinutes = std::clamp(autosaveIntervalMinutes, 1, 120);
            requests.projectSettingsUpdate = next;
        }
        if (state.projectSettingsDirty) {
            ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.25f, 1.0f), "Project settings have unsaved changes.");
        }
        if (ImGui::Button("Save Project Settings")) {
            requests.saveProjectSettings = true;
        }
    }

    ImGui::SeparatorText("Workspace");
    bool prefsChanged = false;
    prefsChanged |= ImGui::SliderFloat("UI Scale", &editorPrefs_.uiScale, 0.75f, 1.75f, "%.2f");
    const char* themeItems[] = {"Reference Dark", "Classic Dark", "High Contrast"};
    int themePreset = std::clamp(editorPrefs_.themePreset, 0, 2);
    if (ImGui::Combo("Theme", &themePreset, themeItems, IM_ARRAYSIZE(themeItems))) {
        editorPrefs_.themePreset = themePreset;
        prefsChanged = true;
    }
    const char* workspaceItems[] = {"Level Editing", "Lighting", "Content"};
    int workspacePreset = std::clamp(editorPrefs_.workspacePreset, 0, 2);
    if (ImGui::Combo("Workspace", &workspacePreset, workspaceItems, IM_ARRAYSIZE(workspaceItems))) {
        editorPrefs_.workspacePreset = workspacePreset;
        prefsChanged = true;
    }
    prefsChanged |= ImGui::DragInt("Layout Version", &editorPrefs_.layoutVersion, 1.0f, 1, 99);
    if (prefsChanged) {
        editorPrefs_.uiScale = std::clamp(editorPrefs_.uiScale, 0.75f, 1.75f);
        editorPrefs_.save(EditorPreferences::defaultPath());
        applyThemePreset();
        applyWorkspacePreset();
    }

    ImGui::End();
    showProjectManager_ = open;
    if (!open && state.project == nullptr) {
        projectManagerDismissed_ = true;
    }
}

void EditorLayer::applyThemePreset() {
    const int preset = std::clamp(editorPrefs_.themePreset, 0, 2);
    if (appliedThemePreset_ == preset) {
        return;
    }
    appliedThemePreset_ = preset;

    if (preset == 1) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(EditorUiMetric::panelPaddingX, EditorUiMetric::panelPaddingY);
        style.FramePadding = ImVec2(EditorUiMetric::rowPaddingX, EditorUiMetric::rowPaddingY);
        style.ItemSpacing = ImVec2(5.0f, 3.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 2.0f);
        style.ScrollbarSize = 10.0f;
        style.WindowRounding = 0.0f;
        style.FrameRounding = preset == 2 ? 0.0f : EditorUiMetric::compactButtonRounding;
        style.GrabRounding = preset == 2 ? 0.0f : EditorUiMetric::compactButtonRounding;
        style.TabRounding = preset == 2 ? 0.0f : EditorUiMetric::compactButtonRounding;
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = preset == 2 ? 1.0f : 0.0f;

        ImVec4* colors = style.Colors;
        if (preset == 2) {
            colors[ImGuiCol_WindowBg] = ImVec4(0.005f, 0.006f, 0.008f, 1.0f);
            colors[ImGuiCol_ChildBg] = ImVec4(0.010f, 0.012f, 0.016f, 1.0f);
            colors[ImGuiCol_PopupBg] = ImVec4(0.010f, 0.012f, 0.016f, 1.0f);
            colors[ImGuiCol_Border] = ImVec4(0.330f, 0.360f, 0.420f, 1.0f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.030f, 0.036f, 0.048f, 1.0f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.120f, 0.150f, 0.220f, 1.0f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.160f, 0.220f, 0.320f, 1.0f);
            colors[ImGuiCol_Header] = ImVec4(0.090f, 0.120f, 0.180f, 1.0f);
            colors[ImGuiCol_HeaderHovered] = ImVec4(0.160f, 0.220f, 0.330f, 1.0f);
            colors[ImGuiCol_HeaderActive] = ImVec4(0.220f, 0.320f, 0.500f, 1.0f);
            colors[ImGuiCol_Button] = ImVec4(0.070f, 0.085f, 0.110f, 1.0f);
            colors[ImGuiCol_ButtonHovered] = ImVec4(0.170f, 0.230f, 0.330f, 1.0f);
            colors[ImGuiCol_ButtonActive] = ImVec4(0.230f, 0.330f, 0.520f, 1.0f);
            colors[ImGuiCol_CheckMark] = ImVec4(0.370f, 0.650f, 1.000f, 1.0f);
            colors[ImGuiCol_SliderGrab] = ImVec4(0.320f, 0.600f, 1.000f, 1.0f);
        } else {
            colors[ImGuiCol_WindowBg] = editorWindowBgColor();
            colors[ImGuiCol_ChildBg] = editorChildBgColor();
            colors[ImGuiCol_PopupBg] = editorPopupBgColor();
            colors[ImGuiCol_Border] = editorBorderColor();
            colors[ImGuiCol_FrameBg] = editorFrameBgColor();
            colors[ImGuiCol_FrameBgHovered] = editorFrameBgHoveredColor();
            colors[ImGuiCol_FrameBgActive] = editorFrameBgActiveColor();
            colors[ImGuiCol_Header] = editorHeaderColor(false);
            colors[ImGuiCol_HeaderHovered] = editorHeaderColor(false, true);
            colors[ImGuiCol_HeaderActive] = editorHeaderColor(true);
            colors[ImGuiCol_Button] = editorButtonColor(false);
            colors[ImGuiCol_ButtonHovered] = editorButtonColor(false, true);
            colors[ImGuiCol_ButtonActive] = editorButtonColor(true);
            colors[ImGuiCol_CheckMark] = editorCheckMarkColor();
            colors[ImGuiCol_SliderGrab] = editorSliderGrabColor();
            colors[ImGuiCol_TitleBg] = editorTitleBgColor(false);
            colors[ImGuiCol_TitleBgActive] = editorTitleBgColor(true);
            colors[ImGuiCol_MenuBarBg] = editorMenuBarBgColor();
            colors[ImGuiCol_Tab] = editorTabColor(false);
            colors[ImGuiCol_TabHovered] = editorTabColor(false, true);
            colors[ImGuiCol_TabActive] = editorTabColor(true);
            colors[ImGuiCol_Separator] = editorSeparatorColor();
            colors[ImGuiCol_ResizeGrip] = editorResizeGripColor();
        }
    }
}

void EditorLayer::applyWorkspacePreset() {
    const int preset = std::clamp(editorPrefs_.workspacePreset, 0, 2);
    if (appliedWorkspacePreset_ == preset) {
        return;
    }
    appliedWorkspacePreset_ = preset;
    visibility_.viewport = true;
    visibility_.sceneHierarchy = true;
    visibility_.inspector = true;
    visibility_.assetBrowser = true;
    visibility_.timeline = true;
    visibility_.log = true;
    visibility_.console = false;
    visibility_.materialEditor = preset == 2;
    visibility_.renderSettings = preset != 2;
    visibility_.debugProfiler = false;
    visibility_.sceneStats = preset == 1;
    visibility_.gpuDiagnostics = false;
    visibility_.renderWorldSettings = false;
}

void EditorLayer::drawRenderWorldSettingsPanel(EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin("Render World Settings")) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Environment");
    const bool hasHdr = state.hdrPath != nullptr && state.hdrPath->has_value();
    ImGui::Text("HDRI: %s", hasHdr ? state.hdrPath->value().filename().string().c_str() : "Procedural / none");
    if (ImGui::Button("Import HDRI")) {
        visibility_.assetBrowser = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Accumulation")) {
        requests.resetAccumulation = AccumulationResetReason::Manual;
    }
    if (state.sceneDocument != nullptr) {
        SceneDocument& document = *state.sceneDocument;
        const SceneDocument before = document;
        EnvironmentLight* environmentLight = nullptr;
        if (Entity* entity = document.registry().entity(document.worldSettings().activeEnvironment);
            entity != nullptr && entity->environmentLight.has_value()) {
            environmentLight = &*entity->environmentLight;
        }
        if (environmentLight == nullptr) {
            for (Entity* entity : document.registry().entities()) {
                if (entity != nullptr && entity->environmentLight.has_value()) {
                    environmentLight = &*entity->environmentLight;
                    document.worldSettings().activeEnvironment = entity->id;
                    break;
                }
            }
        }
        Environment& environment = document.environment();
        bool environmentChanged = false;
        if (environmentLight != nullptr) {
            environmentChanged |= ImGui::Checkbox("Environment Enabled", &environmentLight->enabled);
            environmentChanged |= ImGui::DragFloat("Intensity", &environmentLight->intensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            environmentChanged |= ImGui::DragFloat("Background", &environmentLight->backgroundIntensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            environmentChanged |= ImGui::DragFloat("Rotation", &environmentLight->rotation, 0.01f, -6.28318f, 6.28318f, "%.3f");
        } else {
            environmentChanged |= ImGui::Checkbox("Environment Enabled", &environment.enabled);
            environmentChanged |= ImGui::DragFloat("Intensity", &environment.intensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            environmentChanged |= ImGui::DragFloat("Background", &environment.backgroundIntensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            environmentChanged |= ImGui::DragFloat("Rotation", &environment.rotation, 0.01f, -6.28318f, 6.28318f, "%.3f");
        }
        if (environmentChanged) {
            applySceneWorldComponentsToDocumentSettings(document);
            requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::RendererSettingsOnly, .label = "Edit World Environment"};
        }
    }

    ImGui::SeparatorText("Primary Sun");
    const EntityId sun = state.sceneDocument != nullptr ? state.sceneDocument->primarySun() : EntityId{};
    ImGui::Text("Primary Sun: %s", sun.valid() ? "Assigned" : "Missing");
    if (!sun.valid() && ImGui::Button("Create Primary Sun")) {
        requests.ensurePrimarySun = true;
    }
    if (state.sceneDocument != nullptr) {
        const WorldSettings& world = state.sceneDocument->worldSettings();
        ImGui::Text("Environment Entity: %s", world.activeEnvironment.valid() ? "Assigned" : "None");
        ImGui::Text("Sky Atmosphere: %s", world.skyAtmosphere.valid() ? "Assigned" : "None");
        ImGui::Text("Height Fog: %s", world.heightFog.valid() ? "Assigned" : "None");
        ImGui::Text("Post Process Volume: %s", world.postProcessVolume.valid() ? "Assigned" : "None");
    }

    ImGui::SeparatorText("Sky / Atmosphere");
    if (state.sceneDocument != nullptr) {
        SceneDocument& document = *state.sceneDocument;
        const SceneDocument before = document;
        WorldSettings& world = document.worldSettings();
        RenderSettings& render = document.renderSettings();
        SkyAtmosphere* skyAtmosphere = nullptr;
        if (Entity* entity = document.registry().entity(world.skyAtmosphere);
            entity != nullptr && entity->skyAtmosphere.has_value()) {
            skyAtmosphere = &*entity->skyAtmosphere;
        }
        if (skyAtmosphere == nullptr) {
            for (Entity* entity : document.registry().entities()) {
                if (entity != nullptr && entity->skyAtmosphere.has_value()) {
                    skyAtmosphere = &*entity->skyAtmosphere;
                    world.skyAtmosphere = entity->id;
                    break;
                }
            }
        }
        bool changed = false;
        if (skyAtmosphere != nullptr) {
            changed |= ImGui::Checkbox("Atmosphere Enabled", &skyAtmosphere->enabled);
            changed |= ImGui::DragFloat("Sky Intensity", &skyAtmosphere->skyIntensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            changed |= ImGui::DragFloat("Rayleigh Scale", &skyAtmosphere->rayleighScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
            changed |= ImGui::DragFloat("Mie Scale", &skyAtmosphere->mieScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
            changed |= ImGui::SliderFloat("Mie Anisotropy", &skyAtmosphere->mieAnisotropy, 0.0f, 0.99f, "%.3f");
            changed |= ImGui::SliderFloat("Ground Albedo", &skyAtmosphere->groundAlbedo, 0.0f, 1.0f, "%.3f");
        } else {
            changed |= ImGui::Checkbox("Atmosphere Enabled", &world.atmosphereEnabled);
            changed |= ImGui::DragFloat("Sky Intensity", &render.skyIntensity, 0.02f, 0.0f, 1000.0f, "%.3f");
            changed |= ImGui::DragFloat("Rayleigh Scale", &render.rayleighScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
            changed |= ImGui::DragFloat("Mie Scale", &render.mieScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
            changed |= ImGui::SliderFloat("Mie Anisotropy", &render.mieAnisotropy, 0.0f, 0.99f, "%.3f");
            changed |= ImGui::SliderFloat("Ground Albedo", &render.groundAlbedo, 0.0f, 1.0f, "%.3f");
        }
        if (changed) {
            if (skyAtmosphere != nullptr) {
                world.atmosphereEnabled = skyAtmosphere->enabled;
            }
            applySceneWorldComponentsToDocumentSettings(document);
            requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::RendererSettingsOnly, .label = "Edit Sky Settings"};
        }
    }

    ImGui::SeparatorText("Post Process / GI");
    const RendererSettings& settings = state.renderer.settings();
    ImGui::Text("Denoiser: %s", settings.denoiserEnabled ? "On" : "Off");
    ImGui::Text("TAA: %s", settings.taaEnabled ? "On" : "Off");
    ImGui::Text("ReSTIR GI: %s", settings.restirGiEnabled ? "On" : "Off");
    if (state.sceneDocument != nullptr) {
        SceneDocument& document = *state.sceneDocument;
        const SceneDocument before = document;
        WorldSettings& world = document.worldSettings();
        RenderSettings& render = document.renderSettings();
        PostProcessVolume* postProcess = nullptr;
        if (Entity* entity = document.registry().entity(world.postProcessVolume);
            entity != nullptr && entity->postProcessVolume.has_value()) {
            postProcess = &*entity->postProcessVolume;
        }
        if (postProcess == nullptr) {
            for (Entity* entity : document.registry().entities()) {
                if (entity != nullptr && entity->postProcessVolume.has_value()) {
                    postProcess = &*entity->postProcessVolume;
                    world.postProcessVolume = entity->id;
                    break;
                }
            }
        }
        bool changed = false;
        if (postProcess != nullptr) {
            changed |= ImGui::Checkbox("Post Process Enabled", &postProcess->enabled);
            changed |= ImGui::DragFloat("Exposure", &postProcess->exposureCompensation, 0.02f, -20.0f, 20.0f, "%.2f");
            changed |= ImGui::SliderFloat("Saturation", &postProcess->saturation, 0.0f, 2.0f, "%.3f");
            changed |= ImGui::SliderFloat("Contrast", &postProcess->contrast, 0.0f, 2.0f, "%.3f");
        } else {
            changed |= ImGui::Checkbox("Post Process Enabled", &world.postProcessEnabled);
            changed |= ImGui::DragFloat("Exposure", &render.exposure, 0.02f, -20.0f, 20.0f, "%.2f");
            changed |= ImGui::SliderFloat("Saturation", &render.saturation, 0.0f, 2.0f, "%.3f");
            changed |= ImGui::SliderFloat("Contrast", &render.contrast, 0.0f, 2.0f, "%.3f");
        }
        changed |= ImGui::DragFloat("Indirect Strength", &render.indirectStrength, 0.02f, 0.0f, 20.0f, "%.3f");
        if (changed) {
            if (postProcess != nullptr) {
                world.postProcessEnabled = postProcess->enabled;
            }
            applySceneWorldComponentsToDocumentSettings(document);
            requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::RendererSettingsOnly, .label = "Edit Post Process Settings"};
        }
    }

    ImGui::End();
}

void EditorLayer::drawTimelinePanel(EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin(EditorDockWindowTitle::Timeline)) {
        ImGui::End();
        return;
    }
    auto timelineIconTooltip = [](const char* text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", text);
        }
    };
    if (editorIconButton("TimelinePlay", EditorGlyphIcon::Play, timeline_.playing)) {
        timeline_.playing = true;
        log_.add(EditorLogCategory::Info, "Timeline playback started");
    }
    timelineIconTooltip("Play timeline");
    ImGui::SameLine();
    if (editorIconButton("TimelinePause", EditorGlyphIcon::Pause, !timeline_.playing)) {
        timeline_.playing = false;
        log_.add(EditorLogCategory::Info, "Timeline playback paused");
    }
    timelineIconTooltip("Pause timeline");
    ImGui::SameLine();
    if (editorIconButton("TimelineStop", EditorGlyphIcon::Stop, false)) {
        timeline_.stop();
        log_.add(EditorLogCategory::Info, "Timeline playback stopped");
    }
    timelineIconTooltip("Stop timeline");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(EditorUiMetric::timelineFrameWidth);
    bool sequenceChanged = false;
    sequenceChanged |= ImGui::DragInt("Frame", &timeline_.currentFrame, 1.0f, timeline_.startFrame, timeline_.endFrame);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(EditorUiMetric::timelineRangeFrameWidth);
    sequenceChanged |= ImGui::DragInt("Start", &timeline_.startFrame, 1.0f, 0, timeline_.endFrame);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(EditorUiMetric::timelineRangeFrameWidth);
    sequenceChanged |= ImGui::DragInt("End", &timeline_.endFrame, 1.0f, timeline_.startFrame, 100000);
    if (timeline_.endFrame < timeline_.startFrame) {
        timeline_.endFrame = timeline_.startFrame;
        sequenceChanged = true;
    }
    const int clampedFrame = std::clamp(timeline_.currentFrame, timeline_.startFrame, timeline_.endFrame);
    if (clampedFrame != timeline_.currentFrame) {
        timeline_.currentFrame = clampedFrame;
        sequenceChanged = true;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(EditorUiMetric::timelineFrameRateWidth);
    sequenceChanged |= ImGui::DragInt("FPS", &timeline_.frameRate, 1.0f, 1, 240);
    timeline_.frameRate = std::clamp(timeline_.frameRate, 1, 240);
    ImGui::SameLine();
    int sequenceFrames = std::clamp(editorPrefs_.renderSequenceFramesPerTimelineFrame, 1, 512);
    ImGui::SetNextItemWidth(EditorUiMetric::timelineFrameRateWidth);
    if (ImGui::DragInt("Seq Frames", &sequenceFrames, 1.0f, 1, 512)) {
        editorPrefs_.renderSequenceFramesPerTimelineFrame = std::clamp(sequenceFrames, 1, 512);
        editorPrefs_.save(EditorPreferences::defaultPath());
    }
    timelineIconTooltip("Rendered frames per timeline frame");
    const int range = std::max(1, timeline_.endFrame - timeline_.startFrame);
    const float durationSeconds = static_cast<float>(range) / static_cast<float>(std::max(1, timeline_.frameRate));
    ImGui::SameLine();
    editorIconTextReadout(EditorGlyphIcon::TimelineKey,
        (std::to_string(range) + " fr / " + std::to_string(static_cast<int>(std::round(durationSeconds))) + " s").c_str(),
        ImGui::GetColorU32(editorTimelineDurationTextColor()));
    timelineIconTooltip("Sequence range duration");

    if (editorIconTextButton("TimelineRangeStart", EditorGlyphIcon::Back, "Start")) {
        timeline_.currentFrame = timeline_.startFrame;
        sequenceChanged = true;
    }
    timelineIconTooltip("Jump to sequence start");
    ImGui::SameLine();
    if (editorIconTextButton("TimelineRangeEnd", EditorGlyphIcon::Forward, "End")) {
        timeline_.currentFrame = timeline_.endFrame;
        sequenceChanged = true;
    }
    timelineIconTooltip("Jump to sequence end");
    ImGui::SameLine();
    ImGui::BeginDisabled(timeline_.keyframes().empty());
    if (editorIconTextButton("TimelineFitRangeToKeys", EditorGlyphIcon::Frame, "Fit Keys") && !timeline_.keyframes().empty()) {
        int firstKey = timeline_.keyframes().front().frame;
        int lastKey = timeline_.keyframes().front().frame;
        for (const TimelineTransformKeyframe& key : timeline_.keyframes()) {
            firstKey = std::min(firstKey, key.frame);
            lastKey = std::max(lastKey, key.frame);
        }
        timeline_.startFrame = std::max(0, firstKey);
        timeline_.endFrame = std::max(timeline_.startFrame, lastKey);
        timeline_.currentFrame = std::clamp(timeline_.currentFrame, timeline_.startFrame, timeline_.endFrame);
        sequenceChanged = true;
        log_.add(EditorLogCategory::Scene, "Timeline range fit to transform keys");
    }
    ImGui::EndDisabled();
    timelineIconTooltip("Fit sequence range to existing transform keys");
    ImGui::SameLine();
    if (editorIconTextButton("TimelineRenderSequence", EditorGlyphIcon::Render, "Render Sequence")) {
        requests.renderSequence = true;
        log_.add(EditorLogCategory::Command, "Render sequence queued from Timeline");
    }
    timelineIconTooltip("Queue a render sequence using the current timeline range");
    if (sequenceChanged) {
        requests.timelineChanged = timeline_.serialize();
    }

    const ImVec2 rulerPos = ImGui::GetCursorScreenPos();
    const float rulerWidth = ImGui::GetContentRegionAvail().x;
    const float rulerHeight = EditorUiMetric::timelineRulerHeight;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(rulerPos, ImVec2(rulerPos.x + rulerWidth, rulerPos.y + rulerHeight), ImGui::GetColorU32(editorTimelineRulerBgColor()), EditorUiMetric::compactButtonRounding);
    drawList->AddRect(rulerPos, ImVec2(rulerPos.x + rulerWidth, rulerPos.y + rulerHeight), ImGui::GetColorU32(editorTimelineRulerBorderColor()), EditorUiMetric::compactButtonRounding);
    for (int i = 0; i <= static_cast<int>(EditorUiMetric::timelineRulerTickCount); ++i) {
        const float x = rulerPos.x + (rulerWidth * static_cast<float>(i) / EditorUiMetric::timelineRulerTickCount);
        const int frame = timeline_.startFrame + (range * i / static_cast<int>(EditorUiMetric::timelineRulerTickCount));
        drawList->AddLine(ImVec2(x, rulerPos.y + EditorUiMetric::timelineRulerTickTop), ImVec2(x, rulerPos.y + rulerHeight), ImGui::GetColorU32(editorTimelineRulerTickColor()));
        const std::string label = std::to_string(frame);
        drawList->AddText(ImVec2(x + EditorUiMetric::timelineRulerLabelOffsetX, rulerPos.y + EditorUiMetric::timelineRulerLabelOffsetY), ImGui::GetColorU32(editorTimelineRulerLabelColor()), label.c_str());
    }
    const std::string rangeLabel = std::to_string(timeline_.startFrame) + " - " + std::to_string(timeline_.endFrame);
    drawList->AddText(
        ImVec2(rulerPos.x + EditorUiMetric::timelineRulerRangeTextOffsetX, rulerPos.y + EditorUiMetric::timelineRulerRangeTextOffsetY),
        ImGui::GetColorU32(editorTimelineRangeTextColor()),
        rangeLabel.c_str());
    for (const TimelineTransformKeyframe& key : timeline_.keyframes()) {
        if (key.frame < timeline_.startFrame || key.frame > timeline_.endFrame) {
            continue;
        }
        const float keyT = static_cast<float>(key.frame - timeline_.startFrame) / static_cast<float>(range);
        const float keyX = rulerPos.x + std::clamp(keyT, 0.0f, 1.0f) * rulerWidth;
        drawList->AddTriangleFilled(
            ImVec2(keyX, rulerPos.y + EditorUiMetric::timelineRulerKeyTop),
            ImVec2(keyX - EditorUiMetric::timelineKeyMarkerRadius, rulerPos.y + EditorUiMetric::timelineRulerKeyBottom),
            ImVec2(keyX + EditorUiMetric::timelineKeyMarkerRadius, rulerPos.y + EditorUiMetric::timelineRulerKeyBottom),
            ImGui::GetColorU32(editorTimelineKeyColor(false)));
    }
    const float frameT = static_cast<float>(timeline_.currentFrame - timeline_.startFrame) / static_cast<float>(range);
    const float scrubX = rulerPos.x + std::clamp(frameT, 0.0f, 1.0f) * rulerWidth;
    drawList->AddLine(ImVec2(scrubX, rulerPos.y), ImVec2(scrubX, rulerPos.y + rulerHeight), ImGui::GetColorU32(editorTimelinePlayheadColor()), 2.0f);
    ImGui::Dummy(ImVec2(rulerWidth, rulerHeight));

    const EntityId selected = selection_.entityId();
    const bool canKey = state.sceneDocument != nullptr && selected.valid() && state.sceneDocument->registry().entity(selected) != nullptr;
    ImGui::BeginDisabled(!canKey);
    if (editorIconTextButton("TimelineAddTransformKey", EditorGlyphIcon::TimelineKey, "Add Transform Key") && canKey) {
        const Entity* entity = state.sceneDocument->registry().entity(selected);
        timeline_.addTransformKey(selected, entity->uuid, entity->transform);
        requests.timelineChanged = timeline_.serialize();
        log_.add(EditorLogCategory::Scene, "Added timeline transform key for " + entity->name);
    }
    ImGui::EndDisabled();
    timelineIconTooltip("Add transform key at the current frame");
    ImGui::SameLine();
    if (editorIconTextButton("TimelineSave", EditorGlyphIcon::Save, "Save")) {
        requests.timelineChanged = timeline_.serialize();
        log_.add(EditorLogCategory::Scene, "Timeline saved into scene data");
    }
    timelineIconTooltip("Save timeline into scene data");
    ImGui::SameLine();
    ImGui::BeginDisabled(timeline_.keyframes().empty());
    if (editorIconTextButton("TimelineClear", EditorGlyphIcon::Trash, "Clear") && !timeline_.keyframes().empty()) {
        timeline_.clear();
        timelineSelectedKeyIds_.clear();
        timelineDraggingKeys_ = false;
        requests.timelineChanged = timeline_.serialize();
        log_.add(EditorLogCategory::Scene, "Timeline cleared");
    }
    ImGui::EndDisabled();
    timelineIconTooltip("Clear all timeline keys");

    auto entityNameForUuid = [&](uint64_t uuid) -> std::string {
        if (state.sceneDocument != nullptr) {
            for (const Entity* entity : state.sceneDocument->registry().entities()) {
                if (entity != nullptr && entity->uuid == uuid) {
                    return entity->name;
                }
            }
        }
        return "Entity " + std::to_string(uuid);
    };

    std::vector<uint64_t> tracks;
    for (const TimelineTransformKeyframe& key : timeline_.keyframes()) {
        if (std::find(tracks.begin(), tracks.end(), key.entityUuid) == tracks.end()) {
            tracks.push_back(key.entityUuid);
        }
    }
    if (tracks.empty() && canKey) {
        const Entity* entity = state.sceneDocument->registry().entity(selected);
        if (entity != nullptr) {
            tracks.push_back(entity->uuid);
        }
    }

    ImGui::TextDisabled("Transform keys: %zu", timeline_.keyframes().size());
    auto keySelected = [&](uint64_t keyId) {
        return std::find(timelineSelectedKeyIds_.begin(), timelineSelectedKeyIds_.end(), keyId) != timelineSelectedKeyIds_.end();
    };
    auto pruneMissingSelectedKeys = [&]() {
        timelineSelectedKeyIds_.erase(
            std::remove_if(timelineSelectedKeyIds_.begin(), timelineSelectedKeyIds_.end(), [&](uint64_t keyId) {
                return std::find_if(timeline_.keyframes().begin(), timeline_.keyframes().end(), [&](const TimelineTransformKeyframe& key) {
                    return key.id == keyId;
                }) == timeline_.keyframes().end();
            }),
            timelineSelectedKeyIds_.end());
    };
    auto selectTimelineKey = [&](const TimelineTransformKeyframe& key, bool additive, bool rangeSelect) {
        if (rangeSelect && !timelineSelectedKeyIds_.empty()) {
            const uint64_t anchorId = timelineSelectedKeyIds_.back();
            const auto anchorIt = std::find_if(timeline_.keyframes().begin(), timeline_.keyframes().end(), [&](const TimelineTransformKeyframe& candidate) {
                return candidate.id == anchorId;
            });
            if (anchorIt != timeline_.keyframes().end() && anchorIt->entityUuid == key.entityUuid) {
                const int minFrame = std::min(anchorIt->frame, key.frame);
                const int maxFrame = std::max(anchorIt->frame, key.frame);
                if (!additive) {
                    timelineSelectedKeyIds_.clear();
                }
                for (const TimelineTransformKeyframe& candidate : timeline_.keyframes()) {
                    if (candidate.entityUuid == key.entityUuid && candidate.frame >= minFrame && candidate.frame <= maxFrame && !keySelected(candidate.id)) {
                        timelineSelectedKeyIds_.push_back(candidate.id);
                    }
                }
                return;
            }
        }
        if (!additive) {
            timelineSelectedKeyIds_.clear();
        }
        const auto selectedIt = std::find(timelineSelectedKeyIds_.begin(), timelineSelectedKeyIds_.end(), key.id);
        if (additive && selectedIt != timelineSelectedKeyIds_.end()) {
            timelineSelectedKeyIds_.erase(selectedIt);
        } else if (selectedIt == timelineSelectedKeyIds_.end()) {
            timelineSelectedKeyIds_.push_back(key.id);
        }
    };
    auto beginTimelineKeyDrag = [&](const TimelineTransformKeyframe& key, float mouseX) {
        if (!keySelected(key.id)) {
            timelineSelectedKeyIds_.clear();
            timelineSelectedKeyIds_.push_back(key.id);
        }
        timelineDraggingKeys_ = true;
        timelineDragStartMouseX_ = mouseX;
        timelineDragStartFrames_.clear();
        for (uint64_t keyId : timelineSelectedKeyIds_) {
            const auto it = std::find_if(timeline_.keyframes().begin(), timeline_.keyframes().end(), [&](const TimelineTransformKeyframe& candidate) {
                return candidate.id == keyId;
            });
            if (it != timeline_.keyframes().end()) {
                timelineDragStartFrames_.push_back({keyId, it->frame});
            }
        }
    };
    pruneMissingSelectedKeys();
    if (!timelineSelectedKeyIds_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Selected keys: %zu", timelineSelectedKeyIds_.size());
        ImGui::SameLine();
        if (editorIconTextButton("TimelineDeleteSelectedKeys", EditorGlyphIcon::Trash, "Delete Selected")) {
            for (uint64_t keyId : timelineSelectedKeyIds_) {
                (void)timeline_.removeTransformKeyById(keyId);
            }
            timelineSelectedKeyIds_.clear();
            timelineDraggingKeys_ = false;
            requests.timelineChanged = timeline_.serialize();
            log_.add(EditorLogCategory::Scene, "Deleted selected timeline transform keys");
        }
        timelineIconTooltip("Delete selected timeline keys");
    }
    if (ImGui::BeginTable("TimelineTracks", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::timelineTrackColumnWidth);
        ImGui::TableSetupColumn("Keys");
        ImGui::TableHeadersRow();
        for (uint64_t trackUuid : tracks) {
            ImGui::PushID(static_cast<int>(trackUuid & 0x7fffffff));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, EditorUiMetric::timelineTrackRowVisualHeight);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(entityNameForUuid(trackUuid).c_str());
            ImGui::TableSetColumnIndex(1);
            const ImVec2 lanePos = ImGui::GetCursorScreenPos();
            const float laneWidth = ImGui::GetContentRegionAvail().x;
            const float laneHeight = EditorUiMetric::timelineTrackLaneHeight;
            ImGui::InvisibleButton("TrackLane", ImVec2(laneWidth, laneHeight));
            std::size_t currentFrameKeyIndex = timeline_.keyframes().size();
            for (std::size_t keyIndex = 0; keyIndex < timeline_.keyframes().size(); ++keyIndex) {
                const TimelineTransformKeyframe& key = timeline_.keyframes()[keyIndex];
                if (key.entityUuid == trackUuid && key.frame == timeline_.currentFrame) {
                    currentFrameKeyIndex = keyIndex;
                    break;
                }
            }
            if (ImGui::BeginPopupContextItem("TrackLaneContext")) {
                if (ImGui::MenuItem("Add Transform Key", nullptr, false, canKey)) {
                    const Entity* entity = state.sceneDocument->registry().entity(selected);
                    timeline_.addTransformKey(selected, entity->uuid, entity->transform);
                    requests.timelineChanged = timeline_.serialize();
                    log_.add(EditorLogCategory::Scene, "Added timeline transform key for " + entity->name);
                }
                if (ImGui::MenuItem("Clear Timeline", nullptr, false, !timeline_.keyframes().empty())) {
                    timeline_.clear();
                    timelineSelectedKeyIds_.clear();
                    requests.timelineChanged = timeline_.serialize();
                    log_.add(EditorLogCategory::Scene, "Timeline cleared");
                }
                if (ImGui::MenuItem("Delete Key at Current Frame", nullptr, false, currentFrameKeyIndex < timeline_.keyframes().size())) {
                    if (timeline_.removeTransformKey(currentFrameKeyIndex)) {
                        pruneMissingSelectedKeys();
                        requests.timelineChanged = timeline_.serialize();
                        log_.add(EditorLogCategory::Scene, "Deleted timeline key at current frame");
                    }
                }
                ImGui::EndPopup();
            }
            ImDrawList* laneDrawList = ImGui::GetWindowDrawList();
            laneDrawList->AddRectFilled(lanePos, ImVec2(lanePos.x + laneWidth, lanePos.y + laneHeight), ImGui::GetColorU32(editorTimelineLaneBgColor()), EditorUiMetric::compactButtonRounding);
            laneDrawList->AddRect(lanePos, ImVec2(lanePos.x + laneWidth, lanePos.y + laneHeight), ImGui::GetColorU32(editorTimelineLaneBorderColor()), EditorUiMetric::compactButtonRounding);
            for (std::size_t keyIndex = 0; keyIndex < timeline_.keyframes().size(); ++keyIndex) {
                const TimelineTransformKeyframe& key = timeline_.keyframes()[keyIndex];
                if (key.entityUuid != trackUuid || key.frame < timeline_.startFrame || key.frame > timeline_.endFrame) {
                    continue;
                }
                const float keyT = static_cast<float>(key.frame - timeline_.startFrame) / static_cast<float>(range);
                const float keyX = lanePos.x + std::clamp(keyT, 0.0f, 1.0f) * laneWidth;
                const ImVec2 center(keyX, lanePos.y + laneHeight * 0.5f);
                const bool selectedKey = keySelected(key.id);
                const ImU32 keyColor = ImGui::GetColorU32(editorTimelineKeyColor(selectedKey));
                laneDrawList->AddQuadFilled(
                    ImVec2(center.x, center.y - EditorUiMetric::timelineKeyMarkerRadius),
                    ImVec2(center.x + EditorUiMetric::timelineKeyMarkerRadius, center.y),
                    ImVec2(center.x, center.y + EditorUiMetric::timelineKeyMarkerRadius),
                    ImVec2(center.x - EditorUiMetric::timelineKeyMarkerRadius, center.y),
                    keyColor);
                if (selectedKey) {
                    laneDrawList->AddQuad(
                        ImVec2(center.x, center.y - EditorUiMetric::timelineKeySelectedRadius),
                        ImVec2(center.x + EditorUiMetric::timelineKeySelectedRadius, center.y),
                        ImVec2(center.x, center.y + EditorUiMetric::timelineKeySelectedRadius),
                        ImVec2(center.x - EditorUiMetric::timelineKeySelectedRadius, center.y),
                        ImGui::GetColorU32(editorTimelineKeyOutlineColor()),
                        1.2f);
                }
                ImGui::SetCursorScreenPos(ImVec2(center.x - EditorUiMetric::timelineKeyHitSize * 0.5f, center.y - EditorUiMetric::timelineKeyHitSize * 0.5f));
                const std::string keyButtonId = "TimelineKeyHit##" + std::to_string(key.id);
                ImGui::InvisibleButton(keyButtonId.c_str(), ImVec2(EditorUiMetric::timelineKeyHitSize, EditorUiMetric::timelineKeyHitSize));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("Frame %d - %s", key.frame, entityNameForUuid(key.entityUuid).c_str());
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    selectTimelineKey(key, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
                }
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, EditorUiMetric::timelineKeyDragThreshold)) {
                    if (!timelineDraggingKeys_) {
                        beginTimelineKeyDrag(key, ImGui::GetIO().MouseClickedPos[0].x);
                    }
                    const float framesPerPixel = static_cast<float>(range) / std::max(laneWidth, 1.0f);
                    const int frameDelta = static_cast<int>(std::round((ImGui::GetIO().MousePos.x - timelineDragStartMouseX_) * framesPerPixel));
                    bool dragged = false;
                    for (const auto& [keyId, startFrame] : timelineDragStartFrames_) {
                        dragged |= timeline_.updateTransformKeyFrame(keyId, startFrame + frameDelta);
                    }
                    if (dragged) {
                        requests.timelineChanged = timeline_.serialize();
                    }
                }
            }
            if (timelineDraggingKeys_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                timelineDraggingKeys_ = false;
                timelineDragStartFrames_.clear();
                log_.add(EditorLogCategory::Scene, "Moved selected timeline transform keys");
            }
            const float rowFrameX = lanePos.x + std::clamp(frameT, 0.0f, 1.0f) * laneWidth;
            laneDrawList->AddLine(ImVec2(rowFrameX, lanePos.y), ImVec2(rowFrameX, lanePos.y + laneHeight), ImGui::GetColorU32(editorTimelinePlayheadColor()), 1.0f);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (tracks.empty()) {
        ImGui::TextDisabled("Select an entity and add a transform key to create the first track.");
    }

    if (!timeline_.keyframes().empty()) {
        ImGui::SeparatorText("Keyframes");
        if (ImGui::BeginTable("TimelineKeyEditor", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::timelineKeyEditorTrackWidth);
            ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::timelineKeyEditorFrameWidth);
            ImGui::TableSetupColumn("Position");
            ImGui::TableSetupColumn("Rotation");
            ImGui::TableSetupColumn("Scale");
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::timelineKeyEditorActionWidth);
            ImGui::TableHeadersRow();
            bool stopEditingKeys = false;
            for (std::size_t keyIndex = 0; keyIndex < timeline_.keyframes().size() && !stopEditingKeys; ++keyIndex) {
                const TimelineTransformKeyframe key = timeline_.keyframes()[keyIndex];
                int editedFrame = key.frame;
                Transform editedTransform = key.transform;
                bool changed = false;
                ImGui::PushID(static_cast<int>(keyIndex));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(entityNameForUuid(key.entityUuid).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= ImGui::DragInt("##Frame", &editedFrame, 1.0f, timeline_.startFrame, timeline_.endFrame);
                ImGui::TableSetColumnIndex(2);
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= ImGui::DragFloat3("##Position", &editedTransform.position.x, 0.01f, -100000.0f, 100000.0f, "%.3f");
                ImGui::TableSetColumnIndex(3);
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= ImGui::DragFloat3("##Rotation", &editedTransform.rotationEuler.x, 0.01f, -360.0f, 360.0f, "%.3f");
                ImGui::TableSetColumnIndex(4);
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= ImGui::DragFloat3("##Scale", &editedTransform.scale.x, 0.01f, 0.001f, 1000.0f, "%.3f");
                if (changed && timeline_.updateTransformKey(keyIndex, editedFrame, editedTransform)) {
                    requests.timelineChanged = timeline_.serialize();
                }
                ImGui::TableSetColumnIndex(5);
                if (editorIconButton("TimelineDeleteKey", EditorGlyphIcon::Trash, false, ImVec2(EditorUiMetric::timelineKeyDeleteButtonWidth, EditorUiMetric::timelineKeyDeleteButtonHeight))) {
                    if (timeline_.removeTransformKey(keyIndex)) {
                        requests.timelineChanged = timeline_.serialize();
                        log_.add(EditorLogCategory::Scene, "Deleted timeline transform key");
                        stopEditingKeys = true;
                    }
                }
                timelineIconTooltip("Delete key");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void EditorLayer::drawLogPanel(const EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin(EditorDockWindowTitle::Log)) {
        ImGui::End();
        return;
    }
    static char search[128]{};
    ImGui::InputTextWithHint("##logSearch", "Search log", search, sizeof(search));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        log_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        std::string text;
        for (const EditorLogEntry& entry : log_.entries()) {
            text += "[";
            text += editorLogCategoryName(entry.category);
            text += "] ";
            text += entry.message;
            text += '\n';
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Log File")) {
        const std::filesystem::path path = state.project != nullptr
            ? state.project->savedRoot / "Logs" / "editor.log"
            : std::filesystem::path("out/editor_tools/editor.log");
        if (log_.saveText(path)) {
            log_.add(EditorLogCategory::Info, "Log written to " + path.string());
#if defined(_WIN32)
            ShellExecuteA(nullptr, "open", path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
        } else {
            log_.add(EditorLogCategory::Error, "Failed to write log to " + path.string());
        }
    }
    ImGui::Separator();
    if (state.sceneLoadingStatus != nullptr && !state.sceneLoadingStatus->empty()) {
        ImGui::TextWrapped("[Scene] %s", state.sceneLoadingStatus->c_str());
        if (state.sceneLoadRunning) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel Load")) {
                requests.cancelSceneLoad = true;
            }
        }
    }
    if (state.sceneDirty && state.sceneDocument != nullptr) {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.25f, 1.0f),
            "[Scene] Unsaved changes: %s", state.sceneDocument->lastChangeReason().c_str());
    }
    if (state.undoStack != nullptr) {
        ImGui::Text("[Undo] Next undo: %s", state.undoStack->canUndo() ? state.undoStack->undoLabel() : "None");
        ImGui::Text("[Undo] Next redo: %s", state.undoStack->canRedo() ? state.undoStack->redoLabel() : "None");
    }
    const std::string filter = search;
    for (const EditorLogEntry& entry : log_.entries()) {
        if (!filter.empty() && entry.message.find(filter) == std::string::npos && std::string(editorLogCategoryName(entry.category)).find(filter) == std::string::npos) {
            continue;
        }
        ImVec4 color(0.75f, 0.78f, 0.84f, 1.0f);
        if (entry.category == EditorLogCategory::Error) color = ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
        if (entry.category == EditorLogCategory::Warning) color = ImVec4(0.95f, 0.75f, 0.25f, 1.0f);
        if (entry.category == EditorLogCategory::Scene || entry.category == EditorLogCategory::Project) color = ImVec4(0.50f, 0.75f, 1.0f, 1.0f);
        ImGui::TextColored(color, "[%s] %s", editorLogCategoryName(entry.category), entry.message.c_str());
    }
    ImGui::End();
}

void EditorLayer::drawConsolePanel(EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin("Console")) {
        ImGui::End();
        return;
    }
    static char command[256]{};
    const bool submitted = ImGui::InputTextWithHint("##consoleCommand", "Enter command", command, sizeof(command), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Run") || submitted) && command[0] != '\0') {
        const std::string value = command;
        consoleHistory_.push_back(value);
        if (consoleHistory_.size() > 64) {
            consoleHistory_.erase(consoleHistory_.begin());
        }
        if (executeConsoleCommand(value, state, requests)) {
            log_.add(EditorLogCategory::Command, "Executed console command: " + value);
        } else if (!lastConsoleCommandFailureReason_.empty()) {
            log_.add(EditorLogCategory::Warning, "Console command unavailable: " + value + " (" + lastConsoleCommandFailureReason_ + ")");
        } else {
            log_.add(EditorLogCategory::Warning, "Unknown console command: " + value);
        }
        std::fill(std::begin(command), std::end(command), '\0');
    }
    ImGui::SeparatorText("Commands");
    for (const EditorCommand& registered : defaultEditorCommandRegistry().commands()) {
        ImGui::Text("%s.%s", registered.category.c_str(), registered.name.c_str());
    }
    ImGui::SeparatorText("Unavailable");
    for (const EditorCommandPlaceholder& placeholder : defaultEditorCommandPlaceholders()) {
        ImGui::TextDisabled("%s.%s", placeholder.category.c_str(), placeholder.name.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s\nDisabled: %s", placeholder.description.c_str(), placeholder.disabledReason.c_str());
        }
    }
    ImGui::SeparatorText("History");
    for (auto it = consoleHistory_.rbegin(); it != consoleHistory_.rend(); ++it) {
        ImGui::TextUnformatted(it->c_str());
    }
    ImGui::End();
}

void EditorLayer::updateJobCenterHistory(const EditorRuntimeState& state) {
    auto findEntry = [this](const std::string& key) -> EditorJobHistoryEntry* {
        const auto it = std::find_if(jobHistory_.begin(), jobHistory_.end(), [&](const EditorJobHistoryEntry& entry) {
            return entry.key == key;
        });
        return it != jobHistory_.end() ? &*it : nullptr;
    };

    auto createEntry = [this](std::string key, std::string kind, std::string title) -> EditorJobHistoryEntry& {
        EditorJobHistoryEntry entry;
        entry.id = nextJobHistoryId_++;
        entry.key = std::move(key);
        entry.kind = std::move(kind);
        entry.title = std::move(title);
        jobHistory_.push_back(std::move(entry));
        return jobHistory_.back();
    };

    auto trimHistory = [this]() {
        constexpr size_t maxHistoryEntries = 32;
        while (jobHistory_.size() > maxHistoryEntries) {
            const auto inactiveIt = std::find_if(jobHistory_.begin(), jobHistory_.end(), [](const EditorJobHistoryEntry& entry) {
                return !entry.active;
            });
            if (inactiveIt == jobHistory_.end()) {
                break;
            }
            jobHistory_.erase(inactiveIt);
        }
    };

    const EditorJobCenterState* jobs = state.jobCenter;
    const bool sceneLoadRunning = (jobs != nullptr && jobs->sceneLoadRunning) || state.sceneLoadRunning;
    const float sceneProgress = jobs != nullptr ? jobs->sceneLoadProgress : state.sceneLoadProgress;
    const std::string sceneStatus = jobs != nullptr && !jobs->sceneLoadStatus.empty()
        ? jobs->sceneLoadStatus
        : (state.sceneLoadingStatus != nullptr ? *state.sceneLoadingStatus : std::string{});
    auto finishActiveSceneLoadHistory = [&]() {
        const std::string finalStatus = sceneStatus.empty()
            ? (lastSceneLoadHistoryStatus_.empty() ? std::string("Scene load completed") : lastSceneLoadHistoryStatus_)
            : sceneStatus;
        if (EditorJobHistoryEntry* entry = findEntry(activeSceneLoadHistoryKey_)) {
            entry->status = finalStatus;
            entry->progress = statusLooksFailed(finalStatus) || statusLooksCancelled(finalStatus) ? lastSceneLoadHistoryProgress_ : 1.0f;
            entry->active = false;
            entry->failed = statusLooksFailed(finalStatus);
            entry->cancelled = statusLooksCancelled(finalStatus);
            entry->completed = !entry->failed && !entry->cancelled;
        }
        observedSceneLoadRunningForHistory_ = false;
        activeSceneLoadHistorySerial_ = 0;
        activeSceneLoadHistoryKey_.clear();
    };
    if (sceneLoadRunning) {
        const uint64_t sceneSerial = jobs != nullptr && jobs->sceneLoadJobSerial != 0 ? jobs->sceneLoadJobSerial : nextJobHistoryId_;
        const std::string sceneKey = "scene-load-" + std::to_string(sceneSerial);
        if (observedSceneLoadRunningForHistory_ && activeSceneLoadHistorySerial_ != 0 && activeSceneLoadHistorySerial_ != sceneSerial) {
            finishActiveSceneLoadHistory();
        }
        if (!observedSceneLoadRunningForHistory_ || activeSceneLoadHistoryKey_.empty()) {
            activeSceneLoadHistoryKey_ = sceneKey;
            activeSceneLoadHistorySerial_ = sceneSerial;
            const std::string title = jobs != nullptr && !jobs->sceneLoadTitle.empty() ? jobs->sceneLoadTitle : std::string("Scene Loading");
            (void)createEntry(activeSceneLoadHistoryKey_, "Scene Load", title);
        }
        if (EditorJobHistoryEntry* entry = findEntry(activeSceneLoadHistoryKey_)) {
            entry->serial = sceneSerial;
            entry->title = jobs != nullptr && !jobs->sceneLoadTitle.empty() ? jobs->sceneLoadTitle : std::string("Scene Loading");
            entry->kind = "Scene Load";
            entry->status = sceneStatus.empty() ? "Loading scene" : sceneStatus;
            entry->sourcePath = jobs != nullptr ? jobs->sceneLoadSourcePath : std::filesystem::path{};
            entry->progress = std::clamp(sceneProgress, 0.0f, 1.0f);
            entry->active = true;
            entry->completed = false;
            entry->failed = false;
            entry->cancelled = false;
            entry->hidden = false;
        }
        observedSceneLoadRunningForHistory_ = true;
        lastSceneLoadHistoryStatus_ = sceneStatus;
        lastSceneLoadHistoryProgress_ = std::clamp(sceneProgress, 0.0f, 1.0f);
    } else if (observedSceneLoadRunningForHistory_) {
        finishActiveSceneLoadHistory();
    }

    if (jobs != nullptr && jobs->completedSceneLoadSerial != 0 && jobs->completedSceneLoadSerial != observedSceneLoadResultSerial_) {
        const std::string sceneKey = "scene-load-" + std::to_string(jobs->completedSceneLoadSerial);
        EditorJobHistoryEntry* entry = findEntry(sceneKey);
        if (entry == nullptr) {
            entry = &createEntry(sceneKey, "Scene Load", jobs->completedSceneLoadTitle.empty() ? "Scene Load" : jobs->completedSceneLoadTitle);
        }
        entry->serial = jobs->completedSceneLoadSerial;
        entry->title = jobs->completedSceneLoadTitle.empty() ? "Scene Load" : jobs->completedSceneLoadTitle;
        entry->kind = "Scene Load";
        entry->status = jobs->completedSceneLoadStatus.empty()
            ? (jobs->completedSceneLoadSuccess ? std::string("Completed") : std::string("Failed"))
            : jobs->completedSceneLoadStatus;
        entry->sourcePath = jobs->completedSceneLoadSourcePath;
        entry->progress = jobs->completedSceneLoadSuccess ? 1.0f : entry->progress;
        entry->active = false;
        entry->completed = jobs->completedSceneLoadSuccess;
        entry->failed = !jobs->completedSceneLoadSuccess && !jobs->completedSceneLoadCancelled;
        entry->cancelled = jobs->completedSceneLoadCancelled;
        entry->hidden = false;
        entry->errors.clear();
        entry->warnings.clear();
        if (!jobs->completedSceneLoadError.empty()) {
            entry->errors.push_back(jobs->completedSceneLoadError);
        }
        if (!jobs->completedSceneLoadWarning.empty()) {
            entry->warnings.push_back(jobs->completedSceneLoadWarning);
        }
        entry->workerTotalMs = jobs->completedSceneLoadWorkerTotalMs;
        entry->workerSceneParseMs = jobs->completedSceneLoadWorkerSceneParseMs;
        entry->workerGltfLoadMs = jobs->completedSceneLoadWorkerGltfLoadMs;
        entry->workerDocumentBuildMs = jobs->completedSceneLoadWorkerDocumentBuildMs;
        observedSceneLoadResultSerial_ = jobs->completedSceneLoadSerial;
        if (activeSceneLoadHistorySerial_ == jobs->completedSceneLoadSerial) {
            observedSceneLoadRunningForHistory_ = false;
            activeSceneLoadHistorySerial_ = 0;
            activeSceneLoadHistoryKey_.clear();
        }
    }

    const bool assetImportRunning = jobs != nullptr && jobs->assetImportRunning;
    const size_t queuedImports = jobs != nullptr ? jobs->queuedAssetImports : 0u;
    auto finishActiveAssetImportHistory = [&]() {
        const std::string finalStatus = statusLooksFailed(lastAssetImportHistoryStatus_)
            ? lastAssetImportHistoryStatus_
            : std::string("Completed");
        if (EditorJobHistoryEntry* entry = findEntry(activeAssetImportHistoryKey_)) {
            entry->title = lastAssetImportHistoryTitle_.empty() ? "Asset Import" : lastAssetImportHistoryTitle_;
            entry->status = finalStatus;
            entry->progress = statusLooksFailed(finalStatus) ? lastAssetImportHistoryProgress_ : 1.0f;
            entry->active = false;
            entry->failed = statusLooksFailed(finalStatus);
            entry->cancelled = statusLooksCancelled(finalStatus);
            entry->completed = !entry->failed && !entry->cancelled;
        }
        observedAssetImportRunningForHistory_ = false;
        activeAssetImportHistorySerial_ = 0;
        activeAssetImportHistoryKey_.clear();
    };
    if (assetImportRunning) {
        std::string title = jobs->assetImportTitle.empty() ? "Asset Import" : jobs->assetImportTitle;
        std::string status = jobs->assetImportStatus.empty() ? "Staging metadata" : jobs->assetImportStatus;
        if (queuedImports > 0u) {
            status += " (" + std::to_string(queuedImports) + " queued)";
        }
        const uint64_t importSerial = jobs->assetImportJobSerial != 0 ? jobs->assetImportJobSerial : nextJobHistoryId_;
        const std::string importKey = "asset-import-" + std::to_string(importSerial);
        if (observedAssetImportRunningForHistory_ && activeAssetImportHistorySerial_ != 0 && activeAssetImportHistorySerial_ != importSerial) {
            finishActiveAssetImportHistory();
        }
        if (!observedAssetImportRunningForHistory_ || activeAssetImportHistoryKey_.empty()) {
            activeAssetImportHistoryKey_ = importKey;
            activeAssetImportHistorySerial_ = importSerial;
            (void)createEntry(activeAssetImportHistoryKey_, "Asset Import", title);
        }
        if (EditorJobHistoryEntry* entry = findEntry(activeAssetImportHistoryKey_)) {
            entry->serial = importSerial;
            entry->title = title;
            entry->kind = "Asset Import";
            entry->status = status;
            entry->progress = std::clamp(jobs->assetImportProgress, 0.0f, 1.0f);
            entry->active = true;
            entry->completed = false;
            entry->failed = false;
            entry->cancelled = false;
            entry->hidden = false;
            entry->hasAssetImportRetry = jobs->assetImportCanRetry;
            entry->assetImportPlaceAfterImport = jobs->assetImportPlaceAfterImport;
            entry->assetImportRetry.sourcePath = jobs->assetImportSourcePath;
            entry->assetImportRetry.destinationFolder = jobs->assetImportDestinationFolder;
            entry->assetImportRetry.mode = jobs->assetImportMode;
            entry->assetImportRetry.settings = jobs->assetImportSettings;
            entry->assetReimportGuid = jobs->assetReimportGuid;
        }
        observedAssetImportRunningForHistory_ = true;
        lastAssetImportHistoryTitle_ = title;
        lastAssetImportHistoryStatus_ = status;
        lastAssetImportHistoryProgress_ = std::clamp(jobs->assetImportProgress, 0.0f, 1.0f);
    } else if (observedAssetImportRunningForHistory_) {
        finishActiveAssetImportHistory();
    }

    if (jobs != nullptr && jobs->completedAssetImportSerial != 0 && jobs->completedAssetImportSerial != observedAssetImportResultSerial_) {
        const std::string importKey = "asset-import-" + std::to_string(jobs->completedAssetImportSerial);
        EditorJobHistoryEntry* entry = findEntry(importKey);
        if (entry == nullptr) {
            entry = &createEntry(importKey, "Asset Import", jobs->completedAssetImportTitle.empty() ? "Asset Import" : jobs->completedAssetImportTitle);
        }
        entry->serial = jobs->completedAssetImportSerial;
        entry->title = jobs->completedAssetImportTitle.empty() ? "Asset Import" : jobs->completedAssetImportTitle;
        entry->kind = "Asset Import";
        entry->status = jobs->completedAssetImportStatus.empty()
            ? (jobs->completedAssetImportSuccess ? std::string("Completed") : std::string("Failed"))
            : jobs->completedAssetImportStatus;
        entry->progress = jobs->completedAssetImportSuccess ? 1.0f : entry->progress;
        entry->active = false;
        entry->completed = jobs->completedAssetImportSuccess;
        entry->failed = !jobs->completedAssetImportSuccess;
        entry->cancelled = false;
        entry->hidden = false;
        entry->reportPath = jobs->completedAssetImportReportPath;
        entry->errors = jobs->completedAssetImportErrors;
        entry->warnings = jobs->completedAssetImportWarnings;
        entry->hasAssetImportRetry = jobs->completedAssetImportCanRetry;
        entry->assetImportPlaceAfterImport = jobs->completedAssetImportPlaceAfterImport;
        entry->assetImportRetry.sourcePath = jobs->completedAssetImportSourcePath;
        entry->assetImportRetry.destinationFolder = jobs->completedAssetImportDestinationFolder;
        entry->assetImportRetry.mode = jobs->completedAssetImportMode;
        entry->assetImportRetry.settings = jobs->completedAssetImportSettings;
        entry->assetReimportGuid = jobs->completedAssetReimportGuid;
        entry->workerTotalMs = jobs->completedAssetImportWorkerTotalMs;
        entry->importValidateMs = jobs->completedAssetImportWorkerValidateMs;
        entry->importDirectoryMs = jobs->completedAssetImportWorkerDirectoryMs;
        entry->importInspectMs = jobs->completedAssetImportWorkerInspectMs;
        entry->importWriteMs = jobs->completedAssetImportWorkerWriteMs;
        observedAssetImportResultSerial_ = jobs->completedAssetImportSerial;
        if (activeAssetImportHistorySerial_ == jobs->completedAssetImportSerial) {
            observedAssetImportRunningForHistory_ = false;
            activeAssetImportHistorySerial_ = 0;
            activeAssetImportHistoryKey_.clear();
        }
    }

    if (state.renderJob != nullptr && state.renderJob->serial != 0) {
        const EditorRenderJobStatus& job = *state.renderJob;
        const std::string key = "render-output-" + std::to_string(job.serial);
        EditorJobHistoryEntry* entry = findEntry(key);
        if (entry == nullptr) {
            entry = &createEntry(key, "Render Output", job.title.empty() ? "Render Job" : job.title);
        }
        entry->serial = job.serial;
        entry->title = job.title.empty() ? "Render Job" : job.title;
        entry->kind = "Render Output";
        entry->status = job.status.empty() ? "Waiting" : job.status;
        entry->progress = std::clamp(job.progress, 0.0f, 1.0f);
        entry->active = job.active;
        entry->completed = job.completed;
        entry->failed = job.failed;
        entry->cancelled = job.cancelled;
        entry->outputRoot = job.outputRoot;
        entry->manifestPath = job.manifestPath;
        entry->errors.clear();
        entry->warnings.clear();
        if (job.failed && !entry->status.empty()) {
            entry->errors.push_back(entry->status);
        } else if (job.cancelled && !entry->status.empty()) {
            entry->warnings.push_back(entry->status);
        }
        if (job.active) {
            entry->hidden = false;
        }
    }

    trimHistory();
}

void EditorLayer::drawJobHistoryEntry(size_t index, EditorRequests& requests) {
    if (index >= jobHistory_.size()) {
        return;
    }
    EditorJobHistoryEntry& entry = jobHistory_[index];
    if (entry.hidden) {
        return;
    }

    const char* stateLabel = "Queued";
    ImVec4 stateColor(0.75f, 0.78f, 0.84f, 1.0f);
    if (entry.active) {
        stateLabel = "Active";
        stateColor = ImVec4(0.40f, 0.68f, 1.0f, 1.0f);
    } else if (entry.completed) {
        stateLabel = "Complete";
        stateColor = ImVec4(0.42f, 0.82f, 0.52f, 1.0f);
    } else if (entry.cancelled) {
        stateLabel = "Stopped";
        stateColor = ImVec4(0.95f, 0.65f, 0.30f, 1.0f);
    } else if (entry.failed) {
        stateLabel = "Failed";
        stateColor = ImVec4(0.95f, 0.34f, 0.34f, 1.0f);
    }

    ImGui::PushID(entry.key.c_str());
    ImGui::TextUnformatted(entry.kind.c_str());
    ImGui::SameLine();
    ImGui::TextColored(stateColor, "%s", stateLabel);
    ImGui::TextWrapped("%s", entry.title.empty() ? entry.kind.c_str() : entry.title.c_str());
    if (!entry.status.empty()) {
        ImGui::TextDisabled("%s", entry.status.c_str());
    }
    if (!entry.errors.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.34f, 0.34f, 1.0f), "Error: %s", entry.errors.front().c_str());
        if (entry.errors.size() > 1u) {
            ImGui::TextDisabled("Additional errors: %llu", static_cast<unsigned long long>(entry.errors.size() - 1u));
        }
    }
    if (!entry.warnings.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "Warning: %s", entry.warnings.front().c_str());
        if (entry.warnings.size() > 1u) {
            ImGui::TextDisabled("Additional warnings: %llu", static_cast<unsigned long long>(entry.warnings.size() - 1u));
        }
    }
    if (entry.active || entry.progress > 0.0f) {
        ImGui::ProgressBar(std::clamp(entry.progress, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f));
    }
    if (!entry.sourcePath.empty()) {
        ImGui::TextWrapped("Source: %s", entry.sourcePath.string().c_str());
    }
    if (!entry.outputRoot.empty()) {
        ImGui::TextWrapped("Output: %s", entry.outputRoot.string().c_str());
    }
    if (!entry.reportPath.empty()) {
        ImGui::TextWrapped("Report: %s", entry.reportPath.string().c_str());
    }
    if (!entry.manifestPath.empty()) {
        ImGui::TextWrapped("Manifest: %s", entry.manifestPath.string().c_str());
    }
    if (entry.kind == "Asset Import" &&
        (entry.workerTotalMs > 0.0 || entry.importValidateMs > 0.0 || entry.importDirectoryMs > 0.0 || entry.importInspectMs > 0.0 || entry.importWriteMs > 0.0)) {
        ImGui::TextDisabled(
            "Worker: %.1f ms total, %.1f ms validate, %.1f ms dirs, %.1f ms inspect, %.1f ms write",
            entry.workerTotalMs,
            entry.importValidateMs,
            entry.importDirectoryMs,
            entry.importInspectMs,
            entry.importWriteMs);
    } else if (entry.workerTotalMs > 0.0 || entry.workerSceneParseMs > 0.0 || entry.workerGltfLoadMs > 0.0 || entry.workerDocumentBuildMs > 0.0) {
        ImGui::TextDisabled(
            "Worker: %.1f ms total, %.1f ms parse, %.1f ms glTF/cache, %.1f ms document",
            entry.workerTotalMs,
            entry.workerSceneParseMs,
            entry.workerGltfLoadMs,
            entry.workerDocumentBuildMs);
    }

    bool drewAction = false;
    if (entry.kind == "Asset Import") {
        if (ImGui::SmallButton("Open Content")) {
            visibility_.assetBrowser = true;
        }
        drewAction = true;
        if (!entry.reportPath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open Report")) {
                requests.openFilePath = entry.reportPath;
            }
        }
        if (!entry.errors.empty() || !entry.warnings.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reveal Log")) {
                visibility_.log = true;
            }
        }
        if (entry.hasAssetImportRetry && !entry.assetImportRetry.sourcePath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Retry")) {
                EditorImportAssetRequest retry = entry.assetImportRetry;
                if (retry.mode.empty()) {
                    retry.mode = entry.assetImportPlaceAfterImport ? "ImportAndPlace" : "ImportAsset";
                }
                if (entry.assetImportPlaceAfterImport) {
                    requests.importAndPlace = std::move(retry);
                } else {
                    requests.importAsset = std::move(retry);
                }
            }
        }
        if (!entry.assetReimportGuid.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reimport")) {
                requests.reimportAsset = entry.assetReimportGuid;
            }
        }
    } else if (entry.kind == "Scene Load") {
        if (ImGui::SmallButton("Reveal Log")) {
            visibility_.log = true;
        }
        drewAction = true;
        if (!entry.sourcePath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open Source Folder")) {
                requests.openDirectoryPath = entry.sourcePath.parent_path();
            }
        }
    } else if (entry.kind == "Render Output" && !entry.outputRoot.empty()) {
        if (ImGui::SmallButton("Open Output")) {
            requests.openOutputFolderPath = entry.outputRoot;
        }
        drewAction = true;
        if (!entry.manifestPath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open Manifest")) {
                requests.openFilePath = entry.manifestPath;
            }
        }
        if (entry.failed || entry.cancelled) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reveal Log")) {
                visibility_.log = true;
            }
        }
    }
    if (!entry.active) {
        if (drewAction) {
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Hide")) {
            entry.hidden = true;
        }
    }
    ImGui::Separator();
    ImGui::PopID();
}

void EditorLayer::drawJobCenterPanel(const EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin("Job Center", &visibility_.jobCenter)) {
        ImGui::End();
        return;
    }

    bool hasJob = false;
    const EditorJobCenterState* jobs = state.jobCenter;
    if ((jobs != nullptr && jobs->sceneLoadRunning) || state.sceneLoadRunning) {
        hasJob = true;
        const float progress = jobs != nullptr ? jobs->sceneLoadProgress : state.sceneLoadProgress;
        const std::string status = jobs != nullptr && !jobs->sceneLoadStatus.empty()
            ? jobs->sceneLoadStatus
            : (state.sceneLoadingStatus != nullptr ? *state.sceneLoadingStatus : std::string("Loading scene"));
        ImGui::SeparatorText("Scene Loading");
        ImGui::TextUnformatted(status.c_str());
        ImGui::ProgressBar(std::clamp(progress, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f));
        if (ImGui::SmallButton("Cancel Scene Load")) {
            requests.cancelSceneLoad = true;
        }
    }

    const bool assetImportRunning = jobs != nullptr && jobs->assetImportRunning;
    const size_t queuedImports = jobs != nullptr ? jobs->queuedAssetImports : 0u;
    if (assetImportRunning || queuedImports > 0u) {
        hasJob = true;
        ImGui::SeparatorText("Asset Import");
        if (assetImportRunning) {
            ImGui::TextUnformatted(jobs->assetImportTitle.empty() ? "Asset Import" : jobs->assetImportTitle.c_str());
            ImGui::TextDisabled("%s", jobs->assetImportStatus.empty() ? "Staging metadata" : jobs->assetImportStatus.c_str());
            ImGui::ProgressBar(std::clamp(jobs->assetImportProgress, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f));
        }
        if (queuedImports > 0u) {
            ImGui::TextDisabled("Queued imports: %llu", static_cast<unsigned long long>(queuedImports));
        }
        if (ImGui::SmallButton("Open Content")) {
            visibility_.assetBrowser = true;
        }
    }

    if (state.renderJob != nullptr && state.renderJob->serial != 0) {
        hasJob = true;
        const EditorRenderJobStatus& job = *state.renderJob;
        ImGui::SeparatorText("Render Output");
        ImGui::TextUnformatted(job.title.empty() ? "Render Job" : job.title.c_str());
        ImGui::TextDisabled("%s", job.status.empty() ? "Waiting" : job.status.c_str());
        ImGui::ProgressBar(std::clamp(job.progress, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f));
        if (job.totalFrames > 0) {
            ImGui::TextDisabled("Frames: %d / %d", std::clamp(job.currentFrame, 0, job.totalFrames), job.totalFrames);
        }
        if (!job.outputRoot.empty()) {
            ImGui::TextWrapped("Output: %s", job.outputRoot.string().c_str());
        }
        if (job.active) {
            if (ImGui::SmallButton("Stop Render")) {
                requests.stopRender = true;
            }
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Open Output")) {
            requests.openOutputFolder = true;
        }
    }

    if (!hasJob) {
        ImGui::TextDisabled("No active editor jobs.");
        ImGui::TextDisabled("Scene loads, asset imports, and render output jobs appear here while running.");
    }

    ImGui::SeparatorText("Recent Jobs");
    if (ImGui::SmallButton("Hide Finished")) {
        for (EditorJobHistoryEntry& entry : jobHistory_) {
            if (!entry.active) {
                entry.hidden = true;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Hidden")) {
        jobHistory_.erase(std::remove_if(jobHistory_.begin(), jobHistory_.end(), [](const EditorJobHistoryEntry& entry) {
            return entry.hidden;
        }), jobHistory_.end());
    }

    bool hasVisibleHistory = false;
    for (size_t offset = 0; offset < jobHistory_.size(); ++offset) {
        const size_t index = jobHistory_.size() - 1u - offset;
        if (jobHistory_[index].hidden) {
            continue;
        }
        hasVisibleHistory = true;
        drawJobHistoryEntry(index, requests);
    }
    if (!hasVisibleHistory) {
        ImGui::TextDisabled("No recent job history.");
    }

    ImGui::End();
}

void EditorLayer::drawCommandPalette(EditorRuntimeState& state, EditorRequests& requests) {
    if (!commandPaletteOpen_) {
        return;
    }
    ImGui::OpenPopup("Command Palette");
    ImGui::SetNextWindowSize(ImVec2(620.0f, 430.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Command Palette", &commandPaletteOpen_)) {
        return;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##commandSearch", "Search commands...", commandPaletteSearch_.data(), commandPaletteSearch_.size());
    const std::string filter = [&] {
        std::string value = commandPaletteSearch_.data();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }();

    if (const auto conflicts = defaultEditorCommandRegistry().detectConflicts(); !conflicts.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1.0f), "Shortcut conflicts: %zu", conflicts.size());
    }

    if (ImGui::SmallButton(commandPaletteShortcutEditor_ ? "Commands" : "Shortcuts")) {
        commandPaletteShortcutEditor_ = !commandPaletteShortcutEditor_;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Overrides are saved to editor preferences and displayed in the palette.");

    ImGui::Separator();
    if (ImGui::BeginChild("CommandPaletteResults", ImVec2(0.0f, 0.0f), true)) {
        for (const EditorCommand& command : defaultEditorCommandRegistry().commands()) {
            std::string haystack = command.category + " " + command.name + " " + command.description;
            std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (!filter.empty() && haystack.find(filter) == std::string::npos) {
                continue;
            }
            ImGui::PushID(static_cast<int>(command.id));
            const std::string commandKey = editorCommandPreferenceKey(command);
            const auto overrideIt = editorPrefs_.commandShortcutOverrides.find(commandKey);
            const std::string shortcut = editorCommandShortcutDisplay(command.id, &editorPrefs_);
            const std::string unavailableReason = commandUnavailableReason(command.id, state, selection_);
            const bool commandAvailable = unavailableReason.empty();
            if (commandPaletteShortcutEditor_) {
                ImGui::TextUnformatted((command.category + " / " + command.name).c_str());
                ImGui::SameLine(std::max(320.0f, ImGui::GetWindowContentRegionMax().x - 250.0f));
                std::array<char, 64> shortcutBuffer{};
                shortcut.copy(shortcutBuffer.data(), std::min(shortcut.size(), shortcutBuffer.size() - 1u));
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::InputText("##shortcut", shortcutBuffer.data(), shortcutBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    const std::string next = shortcutBuffer.data();
                    if (next.empty() || next == command.defaultKeybinding.display) {
                        editorPrefs_.commandShortcutOverrides.erase(commandKey);
                    } else {
                        editorPrefs_.commandShortcutOverrides[commandKey] = next;
                    }
                    editorPrefs_.save(EditorPreferences::defaultPath());
                    log_.add(EditorLogCategory::Command, "Updated shortcut display for " + command.name);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset")) {
                    editorPrefs_.commandShortcutOverrides.erase(commandKey);
                    editorPrefs_.save(EditorPreferences::defaultPath());
                }
            } else {
                if (!commandAvailable) {
                    ImGui::BeginDisabled();
                }
                const bool activated = ImGui::Selectable((command.category + " / " + command.name).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort);
                if (!commandAvailable) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine(std::max(360.0f, ImGui::GetWindowContentRegionMax().x - 130.0f));
                if (commandAvailable) {
                    ImGui::TextDisabled("%s", shortcut.empty() ? "" : shortcut.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.78f, 0.60f, 0.45f, 1.0f), "%s", unavailableReason.c_str());
                }
                if (rowHovered) {
                    if (!commandAvailable) {
                        ImGui::SetTooltip(
                            "%s\nDisabled: %s",
                            command.description.empty() ? "Not available" : command.description.c_str(),
                            unavailableReason.c_str());
                    } else if (!command.description.empty()) {
                        ImGui::SetTooltip("%s", command.description.c_str());
                    }
                }
                if (activated && commandAvailable) {
                    if (executeCommandPaletteCommand(command.id, state, requests)) {
                        log_.add(EditorLogCategory::Command, "Command Palette: " + command.name);
                        commandPaletteOpen_ = false;
                        std::fill(commandPaletteSearch_.begin(), commandPaletteSearch_.end(), '\0');
                        ImGui::CloseCurrentPopup();
                    } else {
                        log_.add(EditorLogCategory::Warning, "Command unavailable: " + command.name);
                    }
                } else {
                    if (commandAvailable && !shortcut.empty() && overrideIt != editorPrefs_.commandShortcutOverrides.end() && rowHovered) {
                        ImGui::SetTooltip("Custom shortcut display. Runtime rebinding uses the default command contexts until the next input-system pass.");
                    }
                }
            }
            ImGui::PopID();
        }
        if (!commandPaletteShortcutEditor_) {
            for (const EditorCommandPlaceholder& placeholder : defaultEditorCommandPlaceholders()) {
                std::string haystack = placeholder.category + " " + placeholder.name + " " + placeholder.description + " " + placeholder.disabledReason;
                std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (!filter.empty() && haystack.find(filter) == std::string::npos) {
                    continue;
                }
                ImGui::PushID(placeholder.name.c_str());
                ImGui::BeginDisabled();
                ImGui::Selectable((placeholder.category + " / " + placeholder.name).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort);
                ImGui::EndDisabled();
                ImGui::SameLine(std::max(360.0f, ImGui::GetWindowContentRegionMax().x - 130.0f));
                ImGui::TextColored(ImVec4(0.78f, 0.60f, 0.45f, 1.0f), "%s", placeholder.disabledReason.c_str());
                if (rowHovered) {
                    ImGui::SetTooltip(
                        "%s\nDisabled: %s",
                        placeholder.description.empty() ? "Not available" : placeholder.description.c_str(),
                        placeholder.disabledReason.c_str());
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

bool EditorLayer::executeCommandPaletteCommand(EditorCommandId id, EditorRuntimeState& state, EditorRequests& requests) {
    if (isViewportPanelCommand(id)) {
        visibility_.viewport = true;
        viewportPanel_.executeCommand(id);
        return true;
    }
    switch (id) {
    case EditorCommandId::ProjectManager: requests.showProjectManager = true; return true;
    case EditorCommandId::ProjectSettings:
        if (state.project != nullptr) { requests.showProjectSettings = true; return true; }
        return false;
    case EditorCommandId::CloseProject: requests.closeProject = true; return true;
    case EditorCommandId::NewScene: requests.newScene = true; return true;
    case EditorCommandId::OpenScene:
        if (auto path = openSceneJsonFileDialog()) { requests.openScene = *path; return true; }
        return false;
    case EditorCommandId::SaveScene:
        if (state.scenePath != nullptr && state.scenePath->has_value()) { requests.saveScene = **state.scenePath; return true; }
        if (auto path = saveSceneJsonFileDialog()) { requests.saveSceneAs = *path; return true; }
        return false;
    case EditorCommandId::SaveSceneAs:
        if (auto path = saveSceneJsonFileDialog()) { requests.saveSceneAs = *path; return true; }
        return false;
    case EditorCommandId::SaveAll:
        requests.saveAll = true;
        return true;
    case EditorCommandId::SaveMaterial:
        if (std::optional<AssetGuid> guid = selectedDirtyMaterialAssetGuid(state, selection_)) {
            requests.saveMaterialAsset = *guid;
            visibility_.materialEditor = true;
            return true;
        }
        return false;
    case EditorCommandId::OpenProjectDirectory:
        if (state.project != nullptr) { requests.openProjectDirectory = true; return true; }
        return false;
    case EditorCommandId::OpenLogFolder:
        requests.openLogFolder = true;
        return true;
    case EditorCommandId::OpenAsset:
        visibility_.assetBrowser = true;
        requests.openSelectedAsset = true;
        return true;
    case EditorCommandId::ImportAsset:
        if (auto path = openGltfFileDialog()) { requests.importAsset = EditorImportAssetRequest{.sourcePath = *path}; return true; }
        return false;
    case EditorCommandId::ImportAndPlace:
        if (auto path = openGltfFileDialog()) {
            EditorImportAssetRequest request;
            request.sourcePath = *path;
            request.mode = "ImportAndPlace";
            requests.importAndPlace = std::move(request);
            return true;
        }
        return false;
    case EditorCommandId::ImportSceneAsNewScene:
        if (auto path = openGltfFileDialog()) { requests.importSceneAsNewScene = *path; return true; }
        return false;
    case EditorCommandId::MergeScene:
        if (auto path = openGltfFileDialog()) { requests.mergeScene = *path; return true; }
        return false;
    case EditorCommandId::ImportHdri:
        if (auto path = openHdrFileDialog()) { requests.loadHdr = *path; return true; }
        return false;
    case EditorCommandId::CreateEmptyEntity: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::Empty, {}}; return true;
    case EditorCommandId::CreateCamera: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::Camera, {}}; return true;
    case EditorCommandId::CreatePointLight: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::Light, {}}; return true;
    case EditorCommandId::CreateSpotLight: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::SpotLight, {}}; return true;
    case EditorCommandId::CreateAreaLight: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::AreaLight, {}}; return true;
    case EditorCommandId::CreatePrimarySun: requests.ensurePrimarySun = true; return true;
    case EditorCommandId::CreateEnvironmentLight: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::EnvironmentLight, {}}; return true;
    case EditorCommandId::CreateSkyAtmosphere: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::SkyAtmosphere, {}}; return true;
    case EditorCommandId::CreateHeightFog: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::HeightFog, {}}; return true;
    case EditorCommandId::CreateVolumetricCloud: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::VolumetricCloud, {}}; return true;
    case EditorCommandId::CreatePostProcessVolume: requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::PostProcessVolume, {}}; return true;
    case EditorCommandId::ReloadShaders: requests.reloadShaders = true; requests.resetAccumulation = AccumulationResetReason::ShaderReloaded; return true;
    case EditorCommandId::ShowControls: dockspace_.showControlsWindow(); return true;
    case EditorCommandId::ShowRendererInfo: dockspace_.showRendererInfoWindow(); return true;
    case EditorCommandId::ResetAccumulation: requests.resetAccumulation = AccumulationResetReason::Manual; return true;
    case EditorCommandId::ToggleDenoiser: requests.toggleDenoiser = true; return true;
    case EditorCommandId::ToggleMovingDenoiser:
    case EditorCommandId::ToggleEnvironment:
    case EditorCommandId::ToggleDirectLighting:
    case EditorCommandId::SetDebugBeauty:
    case EditorCommandId::SetDebugDirectLighting:
    case EditorCommandId::SetDebugIndirectLighting:
    case EditorCommandId::SetDebugNormals:
    case EditorCommandId::SetDebugDepth:
    case EditorCommandId::SetDebugMotionVectors:
    case EditorCommandId::SetDebugVariance:
    case EditorCommandId::SetDebugAlbedo:
    case EditorCommandId::SetToneMapperLinear:
    case EditorCommandId::SetToneMapperReinhard:
    case EditorCommandId::SetToneMapperAces:
    case EditorCommandId::SetToneMapperPbrNeutral:
    case EditorCommandId::SetToneMapperAgx:
    case EditorCommandId::ToggleAutoExposure:
        return requestRendererCommand(id, state, requests);
    case EditorCommandId::ToggleSun: requests.togglePrimarySun = true; return true;
    case EditorCommandId::CycleDebugView: requests.toggleDebugView = true; return true;
    case EditorCommandId::CycleIntermediateView: requests.cycleIntermediateView = true; return true;
    case EditorCommandId::RenderCurrentViewport: requests.renderCurrentViewport = true; return true;
    case EditorCommandId::RenderImage: requests.renderImage = true; return true;
    case EditorCommandId::RenderSequence: requests.renderSequence = true; return true;
    case EditorCommandId::Screenshot: requests.renderCurrentViewport = true; return true;
    case EditorCommandId::StopRender: requests.stopRender = true; return true;
    case EditorCommandId::OpenOutputFolder: requests.openOutputFolder = true; return true;
    case EditorCommandId::JobCenter: visibility_.jobCenter = true; return true;
    case EditorCommandId::SaveLayout: requests.saveLayout = true; dockspace_.saveLayout(); return true;
    case EditorCommandId::ResetLayout: requests.resetLayout = true; resetLayout(); return true;
    case EditorCommandId::Undo: requests.undo = true; return true;
    case EditorCommandId::Redo: requests.redo = true; return true;
    case EditorCommandId::ToggleFullscreen: requests.toggleFullscreen = true; return true;
    case EditorCommandId::ViewportFrameSelected:
        if (selection_.entityId().valid()) {
            visibility_.viewport = true;
            requests.focusOnEntity = selection_.entityId();
            return true;
        }
        return false;
    case EditorCommandId::Exit: requests.exit = true; return true;
    case EditorCommandId::CommandPalette: commandPaletteOpen_ = true; return true;
    default:
        return false;
    }
}

bool EditorLayer::executeConsoleCommand(std::string command, EditorRuntimeState& state, EditorRequests& requests) {
    lastConsoleCommandFailureReason_.clear();
    command = normalizeConsoleCommandToken(std::move(command));
    auto matches = [&](EditorCommandId id) {
        const EditorCommand* registered = editorCommand(id);
        if (registered == nullptr) {
            return false;
        }
        const std::string name = normalizeConsoleCommandToken(registered->category + "_" + registered->name);
        const std::string shortName = normalizeConsoleCommandToken(editorCommandName(id));
        return command == name || command == shortName;
    };
    auto placeholderMatches = [&](const EditorCommandPlaceholder& placeholder) {
        const std::string name = normalizeConsoleCommandToken(placeholder.category + "_" + placeholder.name);
        const std::string shortName = normalizeConsoleCommandToken(placeholder.name);
        return command == name || command == shortName;
    };
    auto markUnavailable = [&](EditorCommandId id, const char* fallback) {
        lastConsoleCommandFailureReason_ = commandUnavailableReason(id, state, selection_);
        if (lastConsoleCommandFailureReason_.empty()) {
            lastConsoleCommandFailureReason_ = fallback != nullptr ? fallback : "Command is unavailable in the current context.";
        }
        return false;
    };
    if (matches(EditorCommandId::ProjectManager) || command == "project_manager") { requests.showProjectManager = true; return true; }
    if (matches(EditorCommandId::ProjectSettings) || command == "project_settings") {
        if (state.project == nullptr) {
            return markUnavailable(EditorCommandId::ProjectSettings, "No project is currently open.");
        }
        requests.showProjectSettings = true;
        return true;
    }
    if (matches(EditorCommandId::CloseProject) || command == "close_project") {
        if (state.project == nullptr) {
            return markUnavailable(EditorCommandId::CloseProject, "No project is currently open.");
        }
        requests.closeProject = true;
        return true;
    }
    if (matches(EditorCommandId::NewScene) || command == "new_scene") { requests.newScene = true; return true; }
    if (matches(EditorCommandId::SaveScene) || command == "save_scene") {
        if (state.scenePath == nullptr || !state.scenePath->has_value()) {
            lastConsoleCommandFailureReason_ = "No saved scene path is available; use Save Scene As from the File menu.";
            return false;
        }
        requests.saveScene = **state.scenePath;
        return true;
    }
    if (matches(EditorCommandId::SaveAll) || command == "save_all") { requests.saveAll = true; return true; }
    if (matches(EditorCommandId::SaveMaterial) || command == "save_material") {
        if (std::optional<AssetGuid> guid = selectedDirtyMaterialAssetGuid(state, selection_)) {
            requests.saveMaterialAsset = *guid;
            visibility_.materialEditor = true;
            return true;
        }
        return markUnavailable(EditorCommandId::SaveMaterial, "No dirty linked material asset is selected.");
    }
    if (matches(EditorCommandId::OpenProjectDirectory) || command == "open_project_directory") {
        if (state.project == nullptr) {
            return markUnavailable(EditorCommandId::OpenProjectDirectory, "No project is currently open.");
        }
        requests.openProjectDirectory = true;
        return true;
    }
    if (matches(EditorCommandId::OpenLogFolder) || command == "open_log_folder") {
        requests.openLogFolder = true;
        return true;
    }
    if (matches(EditorCommandId::OpenAsset) || command == "open_asset") {
        visibility_.assetBrowser = true;
        requests.openSelectedAsset = true;
        return true;
    }
    if (matches(EditorCommandId::ResetAccumulation) || command == "reset_accumulation") { requests.resetAccumulation = AccumulationResetReason::Manual; return true; }
    if (matches(EditorCommandId::ReloadShaders) || command == "reload_shaders") { requests.reloadShaders = true; requests.resetAccumulation = AccumulationResetReason::ShaderReloaded; return true; }
    if (matches(EditorCommandId::ShowControls) || command == "show_controls") { dockspace_.showControlsWindow(); return true; }
    if (matches(EditorCommandId::ShowRendererInfo) || command == "show_renderer_info") { dockspace_.showRendererInfoWindow(); return true; }
    if (matches(EditorCommandId::JobCenter) || command == "job_center") { visibility_.jobCenter = true; return true; }
    if (matches(EditorCommandId::CommandPalette) || command == "command_palette") { commandPaletteOpen_ = true; return true; }
    if (matches(EditorCommandId::ToggleDenoiser) || command == "toggle_denoiser") { requests.toggleDenoiser = true; return true; }
    if (matches(EditorCommandId::ToggleSun) || command == "toggle_sun" || command == "toggle_primary_sun") { requests.togglePrimarySun = true; return true; }
    if (matches(EditorCommandId::CycleDebugView) || command == "cycle_debug_view") { requests.toggleDebugView = true; return true; }
    if (matches(EditorCommandId::CycleIntermediateView) || command == "cycle_intermediate_view") { requests.cycleIntermediateView = true; return true; }
    if (matches(EditorCommandId::ToggleMovingDenoiser) || command == "toggle_moving_denoiser") { return requestRendererCommand(EditorCommandId::ToggleMovingDenoiser, state, requests); }
    if (matches(EditorCommandId::ToggleEnvironment) || command == "toggle_environment") { return requestRendererCommand(EditorCommandId::ToggleEnvironment, state, requests); }
    if (matches(EditorCommandId::ToggleDirectLighting) || command == "toggle_direct_lighting") { return requestRendererCommand(EditorCommandId::ToggleDirectLighting, state, requests); }
    if (matches(EditorCommandId::SetDebugBeauty) || command == "set_debug_beauty") { return requestRendererCommand(EditorCommandId::SetDebugBeauty, state, requests); }
    if (matches(EditorCommandId::SetDebugDirectLighting) || command == "set_debug_direct_lighting") { return requestRendererCommand(EditorCommandId::SetDebugDirectLighting, state, requests); }
    if (matches(EditorCommandId::SetDebugIndirectLighting) || command == "set_debug_indirect_lighting") { return requestRendererCommand(EditorCommandId::SetDebugIndirectLighting, state, requests); }
    if (matches(EditorCommandId::SetDebugNormals) || command == "set_debug_normals") { return requestRendererCommand(EditorCommandId::SetDebugNormals, state, requests); }
    if (matches(EditorCommandId::SetDebugDepth) || command == "set_debug_depth") { return requestRendererCommand(EditorCommandId::SetDebugDepth, state, requests); }
    if (matches(EditorCommandId::SetDebugMotionVectors) || command == "set_debug_motion_vectors") { return requestRendererCommand(EditorCommandId::SetDebugMotionVectors, state, requests); }
    if (matches(EditorCommandId::SetDebugVariance) || command == "set_debug_variance") { return requestRendererCommand(EditorCommandId::SetDebugVariance, state, requests); }
    if (matches(EditorCommandId::SetDebugAlbedo) || command == "set_debug_albedo") { return requestRendererCommand(EditorCommandId::SetDebugAlbedo, state, requests); }
    if (matches(EditorCommandId::SetToneMapperLinear) || command == "set_tone_mapper_linear") { return requestRendererCommand(EditorCommandId::SetToneMapperLinear, state, requests); }
    if (matches(EditorCommandId::SetToneMapperReinhard) || command == "set_tone_mapper_reinhard") { return requestRendererCommand(EditorCommandId::SetToneMapperReinhard, state, requests); }
    if (matches(EditorCommandId::SetToneMapperAces) || command == "set_tone_mapper_aces") { return requestRendererCommand(EditorCommandId::SetToneMapperAces, state, requests); }
    if (matches(EditorCommandId::SetToneMapperPbrNeutral) || command == "set_tone_mapper_pbr_neutral") { return requestRendererCommand(EditorCommandId::SetToneMapperPbrNeutral, state, requests); }
    if (matches(EditorCommandId::SetToneMapperAgx) || command == "set_tone_mapper_agx") { return requestRendererCommand(EditorCommandId::SetToneMapperAgx, state, requests); }
    if (matches(EditorCommandId::ToggleAutoExposure) || command == "toggle_auto_exposure") { return requestRendererCommand(EditorCommandId::ToggleAutoExposure, state, requests); }
    if (matches(EditorCommandId::RenderCurrentViewport) || command == "render_current_viewport") { requests.renderCurrentViewport = true; return true; }
    if (matches(EditorCommandId::RenderImage) || command == "render_image") { requests.renderImage = true; return true; }
    if (matches(EditorCommandId::RenderSequence) || command == "render_sequence") { requests.renderSequence = true; return true; }
    if (matches(EditorCommandId::Screenshot) || command == "screenshot") { requests.renderCurrentViewport = true; return true; }
    if (matches(EditorCommandId::StopRender) || command == "stop_render") { requests.stopRender = true; return true; }
    if (matches(EditorCommandId::OpenOutputFolder) || command == "open_output_folder") { requests.openOutputFolder = true; return true; }
    if (matches(EditorCommandId::SaveLayout) || command == "save_layout") { requests.saveLayout = true; dockspace_.saveLayout(); return true; }
    if (matches(EditorCommandId::ResetLayout) || command == "reset_layout") { requests.resetLayout = true; resetLayout(); return true; }
    if (matches(EditorCommandId::Undo) || command == "undo") {
        if (state.undoStack == nullptr || !state.undoStack->canUndo()) {
            return markUnavailable(EditorCommandId::Undo, "Nothing to undo.");
        }
        requests.undo = true;
        return true;
    }
    if (matches(EditorCommandId::Redo) || command == "redo") {
        if (state.undoStack == nullptr || !state.undoStack->canRedo()) {
            return markUnavailable(EditorCommandId::Redo, "Nothing to redo.");
        }
        requests.redo = true;
        return true;
    }
    if (matches(EditorCommandId::ToggleFullscreen) || command == "toggle_fullscreen") { requests.toggleFullscreen = true; return true; }
    if (matches(EditorCommandId::ViewportSelect) || command == "viewport_select") { visibility_.viewport = true; viewportPanel_.executeCommand(EditorCommandId::ViewportSelect); return true; }
    if (matches(EditorCommandId::ViewportMove) || command == "viewport_move") { visibility_.viewport = true; viewportPanel_.executeCommand(EditorCommandId::ViewportMove); return true; }
    if (matches(EditorCommandId::ViewportRotate) || command == "viewport_rotate") { visibility_.viewport = true; viewportPanel_.executeCommand(EditorCommandId::ViewportRotate); return true; }
    if (matches(EditorCommandId::ViewportScale) || command == "viewport_scale") { visibility_.viewport = true; viewportPanel_.executeCommand(EditorCommandId::ViewportScale); return true; }
    if (matches(EditorCommandId::ViewportToggleLocal) || command == "viewport_toggle_local") { visibility_.viewport = true; viewportPanel_.executeCommand(EditorCommandId::ViewportToggleLocal); return true; }
    if (matches(EditorCommandId::ViewportToggleSnap) || command == "viewport_toggle_snap") { visibility_.viewport = true; viewportPanel_.executeCommand(EditorCommandId::ViewportToggleSnap); return true; }
    if (matches(EditorCommandId::ViewportToggleGrid) || command == "viewport_toggle_grid") { visibility_.viewport = true; viewportPanel_.executeCommand(EditorCommandId::ViewportToggleGrid); return true; }
    if (matches(EditorCommandId::ViewportToggleAxes) || command == "viewport_toggle_axes") { visibility_.viewport = true; viewportPanel_.executeCommand(EditorCommandId::ViewportToggleAxes); return true; }
    if (matches(EditorCommandId::ViewportFrameSelected) || command == "viewport_frame_selected") {
        if (!selection_.entityId().valid()) {
            return markUnavailable(EditorCommandId::ViewportFrameSelected, "No entity is selected.");
        }
        visibility_.viewport = true;
        requests.focusOnEntity = selection_.entityId();
        return true;
    }
    if (matches(EditorCommandId::Exit) || command == "exit") { requests.exit = true; return true; }
    for (const EditorCommandPlaceholder& placeholder : defaultEditorCommandPlaceholders()) {
        if (placeholderMatches(placeholder)) {
            lastConsoleCommandFailureReason_ = placeholder.disabledReason;
            return false;
        }
    }
    return false;
}

} // namespace rtv
