#include "rtv/EditorDockspace.h"

#include "rtv/FileDialog.h"
#include "rtv/KeyBindings.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace rtv {

namespace {

std::string activeSceneTitle(const EditorRuntimeState& state) {
    std::filesystem::path path;
    if (state.scenePath != nullptr && state.scenePath->has_value()) {
        path = **state.scenePath;
    } else if (state.gltfPath != nullptr && state.gltfPath->has_value()) {
        path = **state.gltfPath;
    }
    std::string title = path.empty() ? "Untitled Scene" : path.stem().string();
    if (state.sceneDirty) {
        title += "*";
    }
    return title;
}

} // namespace

void EditorDockspace::begin(EditorRuntimeState& state, EditorPanelVisibility& visibility, EditorRequests& requests) {
    drawMainMenu(state, visibility, requests);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("EditorDockspaceHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    if (layoutResetRequested_) {
        buildDefaultLayout();
        layoutResetRequested_ = false;
        requests.resetLayout = false;
    }
    drawHelpWindows();
}

void EditorDockspace::end() {
    ImGui::End();
}

void EditorDockspace::requestResetLayout() {
    layoutResetRequested_ = true;
}

void EditorDockspace::setProfilePath(const std::filesystem::path& scenePath) {
    std::filesystem::path next = scenePath;
    if (next.empty()) {
        return;
    }
    next.replace_extension(".layout.ini");
    if (next == profilePath_) {
        return;
    }
    profilePath_ = std::move(next);
    loadLayout();
}

void EditorDockspace::saveLayout() const {
    if (!profilePath_.empty()) {
        ImGui::SaveIniSettingsToDisk(profilePath_.string().c_str());
    }
}

void EditorDockspace::loadLayout() {
    if (!profilePath_.empty() && std::filesystem::exists(profilePath_)) {
        ImGui::LoadIniSettingsFromDisk(profilePath_.string().c_str());
        layoutResetRequested_ = false;
    }
}

void EditorDockspace::buildDefaultLayout() {
    ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID right = 0;
    ImGuiID bottom = 0;
    ImGuiID rightBottom = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, &bottom, &center);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, &rightBottom, &right);

    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderDockWindow("Hierarchy", right);
    ImGui::DockBuilderDockWindow("Render World Settings", right);
    ImGui::DockBuilderDockWindow("Inspector", rightBottom);
    ImGui::DockBuilderDockWindow("Material Editor", rightBottom);
    ImGui::DockBuilderDockWindow("Content", bottom);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    ImGui::DockBuilderDockWindow("Log", bottom);
    ImGui::DockBuilderDockWindow("Debug / Profiler", bottom);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorDockspace::drawMainMenu(EditorRuntimeState& state, EditorPanelVisibility& visibility, EditorRequests& requests) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Project Manager")) {
            requests.showProjectManager = true;
        }
        if (state.project != nullptr) {
            if (ImGui::MenuItem("Close Project")) {
                requests.closeProject = true;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New Scene")) {
            requests.newScene = true;
        }
        if (ImGui::MenuItem("Open Scene")) {
            visibility.assetBrowser = true;
            if (auto path = openSceneJsonFileDialog()) {
                requests.openScene = *path;
            }
        }
        if (ImGui::MenuItem("Save Scene\tCtrl+S")) {
            visibility.assetBrowser = true;
            if (state.scenePath != nullptr && state.scenePath->has_value()) {
                requests.saveScene = **state.scenePath;
                setProfilePath(**state.scenePath);
                saveLayout();
            } else if (auto path = saveSceneJsonFileDialog()) {
                requests.saveSceneAs = *path;
                setProfilePath(*path);
                saveLayout();
            }
        }
        if (ImGui::MenuItem("Save Scene As")) {
            visibility.assetBrowser = true;
            if (auto path = saveSceneJsonFileDialog()) {
                requests.saveSceneAs = *path;
                setProfilePath(*path);
                saveLayout();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Import Asset")) {
            visibility.assetBrowser = true;
            if (auto path = openGltfFileDialog()) {
                requests.importAsset = *path;
            }
        }
        if (ImGui::MenuItem("Import and Place")) {
            visibility.assetBrowser = true;
            if (auto path = openGltfFileDialog()) {
                requests.importAndPlace = *path;
            }
        }
        if (ImGui::MenuItem("Import Scene as New Scene")) {
            visibility.assetBrowser = true;
            if (auto path = openGltfFileDialog()) {
                requests.importSceneAsNewScene = *path;
            }
        }
        if (ImGui::MenuItem("Merge Scene into Current")) {
            visibility.assetBrowser = true;
            if (auto path = openGltfFileDialog()) {
                requests.mergeScene = *path;
            }
        }
        if (ImGui::MenuItem("Import HDRI")) {
            visibility.assetBrowser = true;
            if (auto path = openHdrFileDialog()) {
                requests.loadHdr = *path;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit\tAlt+F4")) {
            requests.exit = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create")) {
        if (ImGui::MenuItem("Empty Entity")) { requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::Empty, {}}; }
        if (ImGui::MenuItem("Camera")) { requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::Camera, {}}; }
        if (ImGui::MenuItem("Point Light")) { requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::Light, {}}; }
        if (ImGui::MenuItem("Primary Sun")) { requests.ensurePrimarySun = true; }
        ImGui::Separator();
        ImGui::MenuItem("Mesh From Asset", nullptr, false, false);
        ImGui::MenuItem("Prefab", nullptr, false, false);
        ImGui::MenuItem("Volume", nullptr, false, false);
        ImGui::MenuItem("Post Process", nullptr, false, false);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Engine")) {
        ImGui::MenuItem("Play", nullptr, false, false);
        ImGui::MenuItem("Simulate", nullptr, false, false);
        ImGui::MenuItem("Pause", nullptr, false, false);
        ImGui::MenuItem("Stop", nullptr, false, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Reload Shaders\tCtrl+R")) {
            requests.reloadShaders = true;
            requests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        }
        if (ImGui::MenuItem("Controls")) { showControls_ = true; }
        if (ImGui::MenuItem("Renderer Info")) { showRendererInfo_ = true; }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Scene", nullptr, &visibility.viewport);
        ImGui::MenuItem("Hierarchy", nullptr, &visibility.sceneHierarchy);
        ImGui::MenuItem("Render World Settings", nullptr, &visibility.renderWorldSettings);
        ImGui::MenuItem("Inspector", nullptr, &visibility.inspector);
        ImGui::MenuItem("Content", nullptr, &visibility.assetBrowser);
        ImGui::MenuItem("Timeline", nullptr, &visibility.timeline);
        ImGui::MenuItem("Log", nullptr, &visibility.log);
        ImGui::MenuItem("Console", nullptr, &visibility.console);
        ImGui::Separator();
        ImGui::MenuItem("Material Editor", nullptr, &visibility.materialEditor);
        ImGui::MenuItem("Technical Render Settings", nullptr, &visibility.renderSettings);
        ImGui::MenuItem("Debug / Profiler", nullptr, &visibility.debugProfiler);
        ImGui::MenuItem("Scene Stats", nullptr, &visibility.sceneStats);
        ImGui::MenuItem("GPU Diagnostics", nullptr, &visibility.gpuDiagnostics);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        if (ImGui::MenuItem("Reset Accumulation\tR")) {
            requests.resetAccumulation = AccumulationResetReason::Manual;
        }
        if (ImGui::MenuItem("Toggle Denoiser\tF2")) {
            requests.toggleDenoiser = true;
        }
        if (ImGui::MenuItem("Cycle Debug View\tF1")) {
            requests.toggleDebugView = true;
        }
        if (ImGui::MenuItem("Cycle Intermediate Views\tF7")) {
            requests.cycleIntermediateView = true;
        }
        ImGui::Separator();
        ImGui::MenuItem("View Mode", nullptr, false, false);
        ImGui::MenuItem("Quality Preset", nullptr, false, false);
        ImGui::MenuItem("Technical Render Settings", nullptr, &visibility.renderSettings);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Layout")) {
        if (ImGui::MenuItem("Save Layout")) {
            requests.saveLayout = true;
            saveLayout();
        }
        if (ImGui::MenuItem("Reset Layout")) {
            requests.resetLayout = true;
            requestResetLayout();
        }
        ImGui::Separator();
        ImGui::MenuItem("Workspace: Level Editing", nullptr, true, false);
        ImGui::MenuItem("UI Scale", nullptr, false, false);
        ImGui::MenuItem("Theme", nullptr, false, false);
        ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::TextUnformatted(activeSceneTitle(state).c_str());
    const float fps = state.cpuFrameMs > 0.0f ? 1000.0f / state.cpuFrameMs : 0.0f;
    const char* fmt = fps > 0.0f ? "%.0f FPS  %.2f ms" : "-- FPS  %.2f ms";
    const float rightWidth = 132.0f;
    const float availX = ImGui::GetContentRegionAvail().x;
    if (availX > rightWidth) {
        ImGui::SameLine(ImGui::GetCursorPosX() + availX - rightWidth);
    } else {
        ImGui::SameLine();
    }
    ImGui::TextDisabled(fmt, fps, state.cpuFrameMs);

    ImGui::EndMainMenuBar();
}

void EditorDockspace::drawHelpWindows() {
    if (showControls_) {
        ImGui::SetNextWindowSize(ImVec2(520.0f, 360.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Controls", &showControls_)) {
            std::string currentCategory;
            bool categoryOpen = false;
            for (const KeyBinding& binding : allKeyBindings()) {
                if (binding.category != currentCategory) {
                    currentCategory = binding.category;
                    categoryOpen = ImGui::CollapsingHeader(currentCategory.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (categoryOpen) {
                    ImGui::BulletText("%s: %s", binding.key.c_str(), binding.description.c_str());
                }
            }
        }
        ImGui::End();
    }
    if (showRendererInfo_) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 160.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Renderer Info", &showRendererInfo_)) {
            ImGui::TextUnformatted("Hardware RT path tracing, temporal denoising, debug views, glTF loading, HDR environments, and GPU profiling are owned by the existing renderer.");
            ImGui::TextUnformatted("The editor layer submits requests and displays renderer state without replacing the render pipeline.");
        }
        ImGui::End();
    }
}

} // namespace rtv
