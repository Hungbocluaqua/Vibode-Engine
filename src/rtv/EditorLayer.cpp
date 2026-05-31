#include "rtv/EditorLayer.h"

#include "rtv/FileDialog.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace rtv {

EditorRequests EditorLayer::draw(EditorRuntimeState& state) {
    EditorRequests requests;
    state.editorPrefs = &editorPrefs_;
    state.cameraBookmarks = &cameraBookmarks_;
    if (state.sceneDocument != nullptr && state.sceneDocument->sourceGltfPath().has_value()) {
        dockspace_.setProfilePath(*state.sceneDocument->sourceGltfPath());
    } else if (state.gltfPath != nullptr && state.gltfPath->has_value()) {
        dockspace_.setProfilePath(**state.gltfPath);
    }
    dockspace_.begin(state, visibility_, requests);

    if (requests.showProjectManager) {
        showProjectManager_ = true;
        projectManagerDismissed_ = false;
    }
    if (state.project != nullptr) {
        showProjectManager_ = false;
    }
    if (showProjectManager_ || (state.project == nullptr && !projectManagerDismissed_)) {
        drawProjectManager(state, requests);
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
        drawTimelinePanel();
    }
    if (visibility_.log) {
        drawLogPanel(state);
    }
    if (visibility_.console) {
        drawConsolePanel();
    }

    dockspace_.end();
    return requests;
}

void EditorLayer::resetLayout() {
    dockspace_.requestResetLayout();
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

void EditorLayer::drawProjectManager(EditorRuntimeState& state, EditorRequests& requests) {
    if (newProjectName_[0] == '\0') {
        const char* defaultName = "MyProject";
        std::memcpy(newProjectName_.data(), defaultName, std::strlen(defaultName));
    }
    if (newProjectLocation_[0] == '\0') {
        const std::string defaultLocation = (std::filesystem::current_path() / "Projects").string();
        std::memcpy(newProjectLocation_.data(), defaultLocation.data(), std::min(defaultLocation.size(), newProjectLocation_.size() - 1));
    }

    ImGui::SetNextWindowSize(ImVec2(680.0f, 520.0f), ImGuiCond_FirstUseEver);
    bool open = showProjectManager_ || state.project == nullptr;
    if (!ImGui::Begin("Project Manager", &open)) {
        ImGui::End();
        showProjectManager_ = open;
        return;
    }

    ImGui::TextUnformatted("Create or open a project. No-project mode remains available for legacy .rtlevel editing.");
    if (state.project == nullptr) {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.25f, 1.0f), "No project is currently open.");
    } else {
        ImGui::Text("Current project: %s", state.project->name.c_str());
    }

    if (ImGui::BeginTabBar("ProjectManagerTabs")) {
        if (ImGui::BeginTabItem("New Project")) {
            ImGui::InputText("Project Name", newProjectName_.data(), newProjectName_.size());
            ImGui::InputText("Location", newProjectLocation_.data(), newProjectLocation_.size());
            const char* templates[] = {"Empty", "Path-Traced Level Editor Default", "Lighting Test Scene"};
            ImGui::Combo("Template", &newProjectTemplate_, templates, 3);
            ImGui::Checkbox("Create Default Scene", &createDefaultScene_);
            ImGui::Checkbox("Create Default Content Folders", &createDefaultContentFolders_);
            if (ImGui::Button("Create Project")) {
                CreateProjectRequest request;
                request.name = newProjectName_.data();
                request.location = std::filesystem::path(newProjectLocation_.data());
                request.templateName = templates[std::clamp(newProjectTemplate_, 0, 2)];
                request.createDefaultScene = createDefaultScene_;
                request.createDefaultContentFolders = createDefaultContentFolders_;
                requests.createProject = request;
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Open Project")) {
            ImGui::InputText("Project File", openProjectPath_.data(), openProjectPath_.size());
            ImGui::SameLine();
            if (ImGui::Button("Browse")) {
                if (auto path = openProjectFileDialog()) {
                    const std::string value = path->string();
                    std::fill(openProjectPath_.begin(), openProjectPath_.end(), '\0');
                    std::memcpy(openProjectPath_.data(), value.data(), std::min(value.size(), openProjectPath_.size() - 1));
                }
            }
            if (ImGui::Button("Open Project")) {
                const std::filesystem::path path(openProjectPath_.data());
                if (!path.empty()) {
                    requests.openProject = OpenProjectRequest{path};
                }
            }
            if (!editorPrefs_.lastOpenedProject.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Open Last Project")) {
                    requests.openProject = OpenProjectRequest{editorPrefs_.lastOpenedProject};
                }
            }
            ImGui::Checkbox("Open Last Project On Startup", &editorPrefs_.openLastProject);
            ImGui::SeparatorText("Recent Projects");
            if (editorPrefs_.recentProjects.empty()) {
                ImGui::TextDisabled("No recent projects.");
            }
            for (size_t i = 0; i < editorPrefs_.recentProjects.size(); ++i) {
                const std::string& project = editorPrefs_.recentProjects[i];
                const bool missing = !std::filesystem::exists(project);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(std::filesystem::path(project).filename().string().c_str())) {
                    requests.openProject = OpenProjectRequest{project};
                }
                if (missing) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "missing");
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Open")) {
                        requests.openProject = OpenProjectRequest{project};
                    }
                    if (ImGui::MenuItem("Remove from Recent")) {
                        editorPrefs_.removeRecentProject(project);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    if (state.project == nullptr && ImGui::Button("Continue Without Project")) {
        showProjectManager_ = false;
        projectManagerDismissed_ = true;
    }
    if (state.project != nullptr) {
        ImGui::SameLine();
        if (ImGui::Button("Close Project")) {
            requests.closeProject = true;
        }
    }

    ImGui::End();
    showProjectManager_ = open;
    if (!open && state.project == nullptr) {
        projectManagerDismissed_ = true;
    }
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

    ImGui::SeparatorText("Primary Sun");
    const EntityId sun = state.sceneDocument != nullptr ? state.sceneDocument->primarySun() : EntityId{};
    ImGui::Text("Primary Sun: %s", sun.valid() ? "Assigned" : "Missing");
    if (!sun.valid() && ImGui::Button("Create Primary Sun")) {
        requests.ensurePrimarySun = true;
    }

    ImGui::SeparatorText("Post Process / GI");
    const RendererSettings& settings = state.renderer.settings();
    ImGui::Text("Denoiser: %s", settings.denoiserEnabled ? "On" : "Off");
    ImGui::Text("TAA: %s", settings.taaEnabled ? "On" : "Off");
    ImGui::Text("ReSTIR GI: %s", settings.restirGiEnabled ? "On" : "Off");
    ImGui::TextDisabled("Artist-authored world components are planned for the next persistence milestones.");

    ImGui::End();
}

void EditorLayer::drawTimelinePanel() {
    if (!ImGui::Begin("Timeline")) {
        ImGui::End();
        return;
    }
    ImGui::Button("Play");
    ImGui::SameLine();
    ImGui::Button("Pause");
    ImGui::SameLine();
    ImGui::Button("Stop");
    static int frame = 0;
    static int startFrame = 0;
    static int endFrame = 120;
    ImGui::DragInt("Frame", &frame, 1.0f, startFrame, endFrame);
    ImGui::DragInt("Start", &startFrame, 1.0f, 0, endFrame);
    ImGui::DragInt("End", &endFrame, 1.0f, startFrame, 100000);
    ImGui::Button("Add Transform Key");
    ImGui::TextDisabled("Timeline V1 shell. Keyframe persistence is deferred until scene persistence milestones.");
    ImGui::End();
}

void EditorLayer::drawLogPanel(const EditorRuntimeState& state) {
    if (!ImGui::Begin("Log")) {
        ImGui::End();
        return;
    }
    static char search[128]{};
    ImGui::InputTextWithHint("##logSearch", "Search log", search, sizeof(search));
    ImGui::SameLine();
    ImGui::Button("Clear");
    ImGui::SameLine();
    ImGui::Button("Copy");
    ImGui::Separator();
    if (state.sceneLoadingStatus != nullptr && !state.sceneLoadingStatus->empty()) {
        ImGui::TextWrapped("[Scene] %s", state.sceneLoadingStatus->c_str());
    }
    if (state.sceneDirty && state.sceneDocument != nullptr) {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.25f, 1.0f),
            "[Scene] Unsaved changes: %s", state.sceneDocument->lastChangeReason().c_str());
    }
    ImGui::TextDisabled("Persistent categorized logging is deferred; notifications still use the existing notification system.");
    ImGui::End();
}

void EditorLayer::drawConsolePanel() {
    if (!ImGui::Begin("Console")) {
        ImGui::End();
        return;
    }
    static char command[256]{};
    ImGui::InputTextWithHint("##consoleCommand", "Enter command", command, sizeof(command));
    ImGui::SameLine();
    ImGui::Button("Run");
    ImGui::TextDisabled("Console command registry is deferred to the command/keybinding milestone.");
    ImGui::End();
}

} // namespace rtv
