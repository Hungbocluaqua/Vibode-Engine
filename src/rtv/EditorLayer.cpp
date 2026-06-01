#include "rtv/EditorLayer.h"

#include "rtv/EditorCommands.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/FileDialog.h"
#include "rtv/UndoStack.h"

#include <imgui.h>

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

std::filesystem::path canonicalForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return std::filesystem::absolute(path, ec);
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
    dockspace_.begin(state, visibility_, requests);

    if (requests.showProjectManager) {
        showProjectManager_ = true;
        projectManagerDismissed_ = false;
    }
    if (requests.showCommandPalette) {
        commandPaletteOpen_ = true;
    }
    if (state.project != nullptr) {
        showProjectManager_ = false;
    }
    const bool projectManagerGateActive = showProjectManager_ || (state.project == nullptr && !projectManagerDismissed_);
    if (projectManagerGateActive) {
        drawProjectManager(ProjectManagerRuntimeState{
            .project = state.project,
            .sceneLoadingStatus = state.sceneLoadingStatus,
            .sceneLoadRunning = state.sceneLoadRunning,
            .sceneLoadProgress = state.sceneLoadProgress,
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
    if (visibility_.assetBrowser) {
        assetBrowserPanel_.draw(state, selection_, requests);
    }
    if (visibility_.materialEditor) {
        materialEditorPanel_.draw(state, selection_, requests);
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
            captureFocusWindow_ == "Log";
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

void EditorLayer::showRecoveryPrompt(std::filesystem::path markerPath, std::filesystem::path autosavePath) {
    recoveryPromptVisible_ = true;
    recoveryMarkerPath_ = std::move(markerPath);
    recoveryAutosavePath_ = std::move(autosavePath);
}

void EditorLayer::drawRecoveryPrompt(EditorRequests& requests) {
    ImGui::OpenPopup("Recovery Available");
    if (ImGui::BeginPopupModal("Recovery Available", &recoveryPromptVisible_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("A previous editor session marker was found. You can restore the latest autosave if it exists, or discard the recovery marker.");
        ImGui::Separator();
        ImGui::TextWrapped("Marker: %s", recoveryMarkerPath_.string().c_str());
        ImGui::TextWrapped("Autosave: %s", recoveryAutosavePath_.string().c_str());
        const bool canRestore = std::filesystem::exists(recoveryAutosavePath_);
        if (!canRestore) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Restore Autosave", ImVec2(160.0f, 0.0f))) {
            requests.restoreAutosave = true;
            recoveryPromptVisible_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (!canRestore) {
            ImGui::EndDisabled();
        }
        if (!canRestore) {
            ImGui::SameLine();
            ImGui::TextDisabled("No autosave found");
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

    ImGui::SetNextWindowSize(ImVec2(920.0f, 620.0f), ImGuiCond_FirstUseEver);
    bool open = showProjectManager_ || state.project == nullptr;
    if (!ImGui::Begin("Vibode Engine Project Manager", &open)) {
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
#if defined(_WIN32)
                const std::string root = state.project->projectRoot.string();
                ShellExecuteA(nullptr, "open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
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
        static bool autosaveEnabled = true;
        static int autosaveIntervalMinutes = 5;
        static std::string settingsProjectGuid;
        if (settingsProjectGuid != state.project->projectGuid) {
            settingsProjectGuid = state.project->projectGuid;
            autosaveEnabled = state.project->autosaveEnabled;
            autosaveIntervalMinutes = state.project->autosaveIntervalMinutes;
        }
        ImGui::Checkbox("Autosave Enabled", &autosaveEnabled);
        ImGui::DragInt("Autosave Interval Minutes", &autosaveIntervalMinutes, 1.0f, 1, 120);
        if (ImGui::Button("Save Project Settings")) {
            ProjectContext next = *state.project;
            next.autosaveEnabled = autosaveEnabled;
            next.autosaveIntervalMinutes = std::clamp(autosaveIntervalMinutes, 1, 120);
            requests.projectSettingsUpdate = next;
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
        Environment& environment = document.environment();
        bool environmentChanged = false;
        environmentChanged |= ImGui::Checkbox("Environment Enabled", &environment.enabled);
        environmentChanged |= ImGui::DragFloat("Intensity", &environment.intensity, 0.02f, 0.0f, 1000.0f, "%.3f");
        environmentChanged |= ImGui::DragFloat("Background", &environment.backgroundIntensity, 0.02f, 0.0f, 1000.0f, "%.3f");
        environmentChanged |= ImGui::DragFloat("Rotation", &environment.rotation, 0.01f, -6.28318f, 6.28318f, "%.3f");
        if (environmentChanged) {
            requests.sceneSnapshot = EditorSceneSnapshotChange{.before = before, .updateKind = SceneUpdateKind::EnvironmentOnly, .label = "Edit World Environment"};
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
        bool changed = false;
        changed |= ImGui::Checkbox("Atmosphere Enabled", &world.atmosphereEnabled);
        changed |= ImGui::DragFloat("Sky Intensity", &render.skyIntensity, 0.02f, 0.0f, 1000.0f, "%.3f");
        changed |= ImGui::DragFloat("Rayleigh Scale", &render.rayleighScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
        changed |= ImGui::DragFloat("Mie Scale", &render.mieScaleHeight, 10.0f, 100.0f, 50000.0f, "%.0f");
        changed |= ImGui::SliderFloat("Mie Anisotropy", &render.mieAnisotropy, 0.0f, 0.99f, "%.3f");
        changed |= ImGui::SliderFloat("Ground Albedo", &render.groundAlbedo, 0.0f, 1.0f, "%.3f");
        if (changed) {
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
        bool changed = false;
        changed |= ImGui::Checkbox("Post Process Enabled", &world.postProcessEnabled);
        changed |= ImGui::DragFloat("Exposure", &render.exposure, 0.02f, -20.0f, 20.0f, "%.2f");
        changed |= ImGui::SliderFloat("Saturation", &render.saturation, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::SliderFloat("Contrast", &render.contrast, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::DragFloat("Indirect Strength", &render.indirectStrength, 0.02f, 0.0f, 20.0f, "%.3f");
        if (changed) {
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
        } else {
            log_.add(EditorLogCategory::Warning, "Unknown console command: " + value);
        }
        std::fill(std::begin(command), std::end(command), '\0');
    }
    ImGui::SeparatorText("Commands");
    for (const EditorCommand& registered : defaultEditorCommandRegistry().commands()) {
        ImGui::Text("%s.%s", registered.category.c_str(), registered.name.c_str());
    }
    ImGui::SeparatorText("History");
    for (auto it = consoleHistory_.rbegin(); it != consoleHistory_.rend(); ++it) {
        ImGui::TextUnformatted(it->c_str());
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
                const bool activated = ImGui::Selectable((command.category + " / " + command.name).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                ImGui::SameLine(std::max(360.0f, ImGui::GetWindowContentRegionMax().x - 130.0f));
                ImGui::TextDisabled("%s", shortcut.empty() ? "" : shortcut.c_str());
                if (!command.description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", command.description.c_str());
                }
                if (activated) {
                    if (executeCommandPaletteCommand(command.id, state, requests)) {
                        log_.add(EditorLogCategory::Command, "Command Palette: " + command.name);
                        commandPaletteOpen_ = false;
                        std::fill(commandPaletteSearch_.begin(), commandPaletteSearch_.end(), '\0');
                        ImGui::CloseCurrentPopup();
                    } else {
                        log_.add(EditorLogCategory::Warning, "Command unavailable: " + command.name);
                    }
                } else {
                    if (!shortcut.empty() && overrideIt != editorPrefs_.commandShortcutOverrides.end() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Custom shortcut display. Runtime rebinding uses the default command contexts until the next input-system pass.");
                    }
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

bool EditorLayer::executeCommandPaletteCommand(EditorCommandId id, EditorRuntimeState& state, EditorRequests& requests) {
    switch (id) {
    case EditorCommandId::ProjectManager: requests.showProjectManager = true; return true;
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
    case EditorCommandId::ResetAccumulation: requests.resetAccumulation = AccumulationResetReason::Manual; return true;
    case EditorCommandId::ToggleDenoiser: requests.toggleDenoiser = true; return true;
    case EditorCommandId::CycleDebugView: requests.toggleDebugView = true; return true;
    case EditorCommandId::CycleIntermediateView: requests.cycleIntermediateView = true; return true;
    case EditorCommandId::RenderCurrentViewport: requests.renderCurrentViewport = true; return true;
    case EditorCommandId::RenderImage: requests.renderImage = true; return true;
    case EditorCommandId::RenderSequence: requests.renderSequence = true; return true;
    case EditorCommandId::StopRender: requests.stopRender = true; return true;
    case EditorCommandId::OpenOutputFolder: requests.openOutputFolder = true; return true;
    case EditorCommandId::SaveLayout: requests.saveLayout = true; return true;
    case EditorCommandId::ResetLayout: requests.resetLayout = true; resetLayout(); return true;
    case EditorCommandId::Undo: requests.undo = true; return true;
    case EditorCommandId::Redo: requests.redo = true; return true;
    case EditorCommandId::Exit: requests.exit = true; return true;
    case EditorCommandId::CommandPalette: commandPaletteOpen_ = true; return true;
    default:
        return false;
    }
}

bool EditorLayer::executeConsoleCommand(std::string command, EditorRuntimeState& state, EditorRequests& requests) {
    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char ch) {
        return ch == ' ' || ch == '.' || ch == '-' ? '_' : static_cast<char>(std::tolower(ch));
    });
    auto matches = [&](EditorCommandId id) {
        const EditorCommand* registered = editorCommand(id);
        if (registered == nullptr) {
            return false;
        }
        std::string name = registered->category + "_" + registered->name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return ch == ' ' || ch == '.' || ch == '-' ? '_' : static_cast<char>(std::tolower(ch));
        });
        return command == name || command == std::string(editorCommandName(id));
    };
    if (matches(EditorCommandId::ProjectManager) || command == "project_manager") { requests.showProjectManager = true; return true; }
    if (matches(EditorCommandId::NewScene) || command == "new_scene") { requests.newScene = true; return true; }
    if (matches(EditorCommandId::SaveScene) || command == "save_scene") {
        if (state.scenePath == nullptr || !state.scenePath->has_value()) {
            return false;
        }
        requests.saveScene = **state.scenePath;
        return true;
    }
    if (matches(EditorCommandId::ResetAccumulation) || command == "reset_accumulation") { requests.resetAccumulation = AccumulationResetReason::Manual; return true; }
    if (matches(EditorCommandId::ReloadShaders) || command == "reload_shaders") { requests.reloadShaders = true; requests.resetAccumulation = AccumulationResetReason::ShaderReloaded; return true; }
    if (matches(EditorCommandId::Undo) || command == "undo") { requests.undo = true; return true; }
    if (matches(EditorCommandId::Redo) || command == "redo") { requests.redo = true; return true; }
    if (matches(EditorCommandId::Exit) || command == "exit") { requests.exit = true; return true; }
    return false;
}

} // namespace rtv
