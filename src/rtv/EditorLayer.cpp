#include "rtv/EditorLayer.h"

#include "rtv/EditorCommands.h"
#include "rtv/FileDialog.h"
#include "rtv/UndoStack.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#include <Shellapi.h>
#endif

namespace rtv {

EditorRequests EditorLayer::draw(EditorRuntimeState& state) {
    EditorRequests requests;
    state.editorPrefs = &editorPrefs_;
    state.cameraBookmarks = &cameraBookmarks_;
    state.log = &log_;
    state.timeline = &timeline_;
    ImGui::GetIO().FontGlobalScale = std::clamp(editorPrefs_.uiScale, 0.75f, 1.75f);
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
    if (recoveryPromptVisible_) {
        drawRecoveryPrompt(requests);
    }
    if (state.sceneLoadRunning) {
        drawSceneLoadingOverlay(state, requests);
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

    dockspace_.end();
    return requests;
}

void EditorLayer::resetLayout() {
    dockspace_.requestResetLayout();
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
        style.WindowPadding = ImVec2(5.0f, 4.0f);
        style.FramePadding = ImVec2(5.0f, 2.0f);
        style.ItemSpacing = ImVec2(5.0f, 3.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 2.0f);
        style.ScrollbarSize = 10.0f;
        style.WindowRounding = 0.0f;
        style.FrameRounding = preset == 2 ? 0.0f : 1.0f;
        style.GrabRounding = preset == 2 ? 0.0f : 1.0f;
        style.TabRounding = preset == 2 ? 0.0f : 1.0f;
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
            colors[ImGuiCol_WindowBg] = ImVec4(0.018f, 0.020f, 0.023f, 1.0f);
            colors[ImGuiCol_ChildBg] = ImVec4(0.026f, 0.028f, 0.031f, 1.0f);
            colors[ImGuiCol_PopupBg] = ImVec4(0.020f, 0.022f, 0.026f, 1.0f);
            colors[ImGuiCol_Border] = ImVec4(0.115f, 0.120f, 0.130f, 0.85f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.060f, 0.064f, 0.070f, 1.0f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.100f, 0.120f, 0.145f, 1.0f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.125f, 0.155f, 0.200f, 1.0f);
            colors[ImGuiCol_Header] = ImVec4(0.080f, 0.085f, 0.092f, 1.0f);
            colors[ImGuiCol_HeaderHovered] = ImVec4(0.120f, 0.145f, 0.180f, 1.0f);
            colors[ImGuiCol_HeaderActive] = ImVec4(0.150f, 0.190f, 0.250f, 1.0f);
            colors[ImGuiCol_Button] = ImVec4(0.070f, 0.075f, 0.084f, 1.0f);
            colors[ImGuiCol_ButtonHovered] = ImVec4(0.105f, 0.125f, 0.155f, 1.0f);
            colors[ImGuiCol_ButtonActive] = ImVec4(0.135f, 0.170f, 0.230f, 1.0f);
            colors[ImGuiCol_CheckMark] = ImVec4(0.300f, 0.560f, 0.980f, 1.0f);
            colors[ImGuiCol_SliderGrab] = ImVec4(0.260f, 0.500f, 0.900f, 1.0f);
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
    visibility_.debugProfiler = preset == 0;
    visibility_.sceneStats = preset == 1;
    visibility_.gpuDiagnostics = false;
    visibility_.renderWorldSettings = preset != 2;
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
    if (!ImGui::Begin("Timeline")) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Play")) {
        timeline_.playing = true;
        log_.add(EditorLogCategory::Info, "Timeline playback started");
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause")) {
        timeline_.playing = false;
        log_.add(EditorLogCategory::Info, "Timeline playback paused");
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        timeline_.stop();
        log_.add(EditorLogCategory::Info, "Timeline playback stopped");
    }
    ImGui::DragInt("Frame", &timeline_.currentFrame, 1.0f, timeline_.startFrame, timeline_.endFrame);
    ImGui::DragInt("Start", &timeline_.startFrame, 1.0f, 0, timeline_.endFrame);
    ImGui::DragInt("End", &timeline_.endFrame, 1.0f, timeline_.startFrame, 100000);
    if (timeline_.endFrame < timeline_.startFrame) {
        timeline_.endFrame = timeline_.startFrame;
    }
    const EntityId selected = selection_.entityId();
    const bool canKey = state.sceneDocument != nullptr && selected.valid() && state.sceneDocument->registry().entity(selected) != nullptr;
    if (ImGui::Button("Add Transform Key", ImVec2(0.0f, 0.0f)) && canKey) {
        const Entity* entity = state.sceneDocument->registry().entity(selected);
        timeline_.addTransformKey(selected, entity->uuid, entity->transform);
        requests.timelineChanged = timeline_.serialize();
        log_.add(EditorLogCategory::Scene, "Added timeline transform key for " + entity->name);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Timeline")) {
        requests.timelineChanged = timeline_.serialize();
        log_.add(EditorLogCategory::Scene, "Timeline saved into scene data");
    }
    ImGui::Text("Transform keys: %zu", timeline_.keyframes().size());
    for (const TimelineTransformKeyframe& key : timeline_.keyframes()) {
        ImGui::Text("Frame %d  Entity UUID %llu", key.frame, static_cast<unsigned long long>(key.entityUuid));
    }
    ImGui::End();
}

void EditorLayer::drawLogPanel(const EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin("Log")) {
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
